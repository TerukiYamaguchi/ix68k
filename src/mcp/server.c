#include "app_internal.h"
#include "platform/platform.h"
#include "bus/x68k_bus.h"
#include "mcp/commands.h"
#include "mcp/internal.h"
#include "mcp/sock_path.h"
#include "mcp/port.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* MCP ソケットサーバー: 接続、フレーム分割、応答送信、非同期コマンドpollだけを担当する。
 *
 * ワイヤのフレーミング（v3以降共通。現行のプロトコル版は
 * commands.c の MCP_PROTO_VERSION）: リクエスト・レスポンスとも
 * <uint32 BE payload_len><payload> で、payload は "<tag> <本文>\n" + 生バイト列。
 * tagはクライアントが生成する任意トークン（空白なし）で、サーバはそのまま
 * レスポンスの先頭へエコーするだけ（サーバ側に状態は持たない）。tagの区切り・
 * ヘッダ行/本文の区切りを server.c がここで剥がすので、commands.c 側は
 * v2 までと同じ "<GROUP> [<ACTION>] [args...]" だけを見ればよい（tagの概念を
 * 一切知らない）。 */
#ifdef _WIN32
typedef SOCKET mcp_sock_t;
#define MCP_INVALID_SOCK INVALID_SOCKET
#define mcp_closesock(s) closesocket(s)
#if !defined(_SSIZE_T_DEFINED)
typedef long ssize_t;
#define _SSIZE_T_DEFINED
#endif
typedef WSAPOLLFD mcp_pollfd_t;
#define mcp_poll(fds, n, ms) WSAPoll(fds, n, ms)
#else
typedef int mcp_sock_t;
#define MCP_INVALID_SOCK     (-1)
#define mcp_closesock(s)     close(s)
typedef struct pollfd mcp_pollfd_t;
#define mcp_poll(fds, n, ms) poll(fds, n, ms)
#endif

#define MCP_OUT_BUF_SIZE      16384
/* 受信フレームの上限。現行コマンドで最大なのは MEM WRITE の256バイト本体
 * ＋ヘッダ行数十バイトなので、これで十分な余裕がある。 */
#define MCP_FRAME_MAX         4096
#define MCP_TAG_MAX           32
/* mcp_open_listener() が診断ログ用に組み立てるエンドポイント表記の上限
 * （POSIX: ソケットパス／Windows: "127.0.0.1:65535" 程度）。 */
#define MCP_ENDPOINT_DESC_MAX 128
/* 応答を送り切るまでの上限。通常は即座に送り切れるので、この締切に達するのは
 * クライアントが受信を止めたまま生き続けている場合だけ。無期限に待つと
 * エミュレータのメインループごと止まるため打ち切る。 */
#define MCP_SEND_TIMEOUT_MS   5000
/* listen()の待ち行列長。接続は1本ずつしか処理しないため、DBG WAIT等で処理が長引く間、
 * 外部デバッガが再試行のたびに張る接続（応答待ちを諦めて閉じたものも、accept
 * されるまで行列に残る）が溢れて接続拒否になるのを避ける。 */
#define MCP_LISTEN_BACKLOG    128
/* 1回のsend()に渡す最大バイト数（Windowsのsend()は長さがint）。 */
#define MCP_SEND_CHUNK_MAX    (1 << 20)

static mcp_sock_t s_mcp_srv = MCP_INVALID_SOCK;
static mcp_sock_t s_mcp_cli = MCP_INVALID_SOCK;
static uint8_t s_mcp_out[MCP_OUT_BUF_SIZE];
static mcp_context_t s_mcp_context = {
    .output = s_mcp_out,
    .output_capacity = MCP_OUT_BUF_SIZE,
};

/* 受信中フレームの状態。長さプレフィックス4バイト→payload本体の2段。 */
static uint8_t s_mcp_rx_payload[MCP_FRAME_MAX];
static uint32_t s_mcp_rx_have;
static uint32_t s_mcp_rx_frame_len;
static uint8_t s_mcp_rx_lenbuf[4];
static int s_mcp_rx_lenbuf_have;
static int s_mcp_rx_in_payload;

/* 応答へ載せるtag。現在処理中（または非同期待ち中）のリクエストのtagを
 * mcp_handle_frame() が設定し、mcp_reply()/mcp_reply_bin() が読む。 */
static char s_mcp_reply_tag[MCP_TAG_MAX] = "";

void mcp_output_append(uint8_t ch) {
    if (s_mcp_context.output_len < s_mcp_context.output_capacity)
        s_mcp_context.output[s_mcp_context.output_len++] = ch;
}

static void mcp_drop_client(void);

static int mcp_io_would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/* 1tick内で複数コマンドを処理する場合の送信締切。tickごとに使い回すことで、
 * コマンドN個が詰まっても最大待ちが MCP_SEND_TIMEOUT_MS*N ではなく
 * MCP_SEND_TIMEOUT_MS のままになる（M-5）。mcp_tick() の先頭で無効化する。 */
static uint64_t s_mcp_send_deadline;
static int s_mcp_send_deadline_valid;

static uint64_t mcp_send_deadline(void) {
    if (!s_mcp_send_deadline_valid) {
        s_mcp_send_deadline = pal_get_ticks_us() + (uint64_t)MCP_SEND_TIMEOUT_MS * 1000;
        s_mcp_send_deadline_valid = 1;
    }
    return s_mcp_send_deadline;
}

/* 応答を全バイト送り切る。クライアントソケットは非ブロッキングなので、1回の
 * send() では送信バッファ長（macOSのAF_UNIXは8KB、WindowsのTCPも同様に
 * 送信バッファで黙って切れる）で黙って切れる——サイズの
 * 大きい応答は容易にこれを超える。書き込み可能になるまでpoll()で待ちながら
 * ループし、締切に達したら諦めて0を返す。 */
static int mcp_send_all(const void *data, size_t len) {
    if (s_mcp_cli == MCP_INVALID_SOCK)
        return 0;
    const char *p = (const char *)data;
    size_t sent = 0;
    uint64_t deadline = mcp_send_deadline();
    while (sent < len) {
        /* Windowsのsend()は長さがintなので、1回の送信量を上限付きに刻む。 */
        size_t chunk = len - sent;
        if (chunk > MCP_SEND_CHUNK_MAX)
            chunk = MCP_SEND_CHUNK_MAX;
        ssize_t n = send(s_mcp_cli, p + sent, (int)chunk, 0);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == 0 || !mcp_io_would_block())
            break; /* 相手が切断済み（SIGPIPEはmain.cで無視）。 */
        uint64_t now = pal_get_ticks_us();
        if (now >= deadline)
            break;
        mcp_pollfd_t pfd;
        pfd.fd = s_mcp_cli;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int ready = mcp_poll(&pfd, 1, (int)((deadline - now) / 1000) + 1);
#ifndef _WIN32
        if (ready < 0 && errno == EINTR)
            continue; /* send側と同じくシグナル割り込みでは諦めない。 */
#endif
        if (ready <= 0)
            break;
    }
    if (sent < len) {
        /* 途中まで送った応答はフレーム境界の途中で終わっているため、この接続は
         * もう同期が取れない（次の応答の断片が連結される）。切り離して
         * クライアントにEOFを返し、やり直させる。 */
        pal_log("MCP: 応答を送り切れませんでした (%zu/%zu バイト)。接続を切ります\n", sent,
                len);
        mcp_drop_client();
    }
    return sent == len;
}

static int mcp_send_frame_prefix(uint32_t total_len) {
    uint8_t buf[4] = {
        (uint8_t)(total_len >> 24),
        (uint8_t)(total_len >> 16),
        (uint8_t)(total_len >> 8),
        (uint8_t)total_len,
    };
    return mcp_send_all(buf, sizeof(buf));
}

/* text は呼び出し側が組み立てた完結ヘッダ行（"OK\n" / "ERR <CODE> <msg>\n" 等、
 * 末尾改行込み）。1フレーム = <len><tag> <text> として送る。 */
int mcp_reply(const char *text, size_t len) {
    size_t taglen = strlen(s_mcp_reply_tag);
    if (!mcp_send_frame_prefix((uint32_t)(taglen + 1 + len)))
        return 0;
    if (!mcp_send_all(s_mcp_reply_tag, taglen))
        return 0;
    if (!mcp_send_all(" ", 1))
        return 0;
    return mcp_send_all(text, len);
}

/* 成功応答＋生バイナリ本体を1フレームで送る（旧base64応答の置き換え）。 */
int mcp_reply_bin(const uint8_t *data, size_t len) {
    size_t taglen = strlen(s_mcp_reply_tag);
    if (!mcp_send_frame_prefix((uint32_t)(taglen + 1 + 3 + len)))
        return 0;
    if (!mcp_send_all(s_mcp_reply_tag, taglen))
        return 0;
    if (!mcp_send_all(" OK\n", 4))
        return 0;
    if (len && !mcp_send_all(data, len))
        return 0;
    return 1;
}

static void mcp_rx_reset(void) {
    s_mcp_rx_lenbuf_have = 0;
    s_mcp_rx_in_payload = 0;
    s_mcp_rx_have = 0;
}

/* クライアントを切り離す。受信中フレームの状態も捨て、その断片が次の
 * クライアントの最初のコマンドへ連結されないようにする。 */
static void mcp_drop_client(void) {
    if (s_mcp_cli != MCP_INVALID_SOCK) {
        mcp_closesock(s_mcp_cli);
        s_mcp_cli = MCP_INVALID_SOCK;
    }
    mcp_rx_reset();
    mcp_commands_client_disconnected(&s_mcp_context);
}

#ifndef _WIN32
static int mcp_socket_is_live(const char *path) {
    mcp_sock_t probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe == MCP_INVALID_SOCK)
        return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    int live = connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    mcp_closesock(probe);
    return live;
}

/* mcp_cleanup() が unlink して良いのは、自分がbind/listenに成功した
 * ソケットファイルだけ。この判定が無いと、同じパスへ2台目を
 * 起動して2台目だけ終了したとき、1台目のソケットファイルを消してしまう。
 * TCP（Windows経路）にはポート番号がファイルのように残留する
 * ことが無いため、この仕組みごとPOSIX側限定にする。 */
static int s_mcp_path_owned;
#endif

/* listenerを開く処理をまとめる。#ifdef _WIN32 の分岐を
 * mcp_init() の中に散らすと読めなくなるため、関数ごと2実装に分ける
 * （POSIX/AF_UNIXとWindows/AF_INETは共通部分よりアドレス型・生成・事前
 * チェック・後始末の違いの方が大きい）。成功時1、失敗時0を返す（理由は
 * 呼び出し前にログ済み）。desc には成功ログ用の人間可読なエンドポイント名を
 * 入れる。 */
static int mcp_open_listener(mcp_sock_t *out, char *desc, size_t desc_size) {
#ifdef _WIN32
    uint16_t port = mcp_port();
    if (port == 0) {
        pal_log("MCP: IX68K_MCP_PORT の値が不正です（%u..%u の整数を指定してください）。"
                "MCPサーバは起動しません。\n",
                (unsigned)MCP_PORT_MIN, (unsigned)MCP_PORT_MAX);
        return 0;
    }
    mcp_sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == MCP_INVALID_SOCK)
        return 0;
    /* SO_REUSEADDR は絶対に設定しない。Windowsでは他プロセスがbind済みの
     * アドレスを奪えてしまい、bind失敗による多重起動検出（この設計の中心）が
     * 壊れる。逆に SO_EXCLUSIVEADDRUSE を立てて奪われる側にもならないように
     * する。これは保険ではなく必須要件なので、失敗したらlistenerを開かない */
    BOOL excl = TRUE;
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl)) !=
        0) {
        pal_log("MCP: SO_EXCLUSIVEADDRUSE を設定できません（WSA error %d）。"
                "他プロセスにポートを奪われる恐れがあるためMCPサーバは起動しません。\n",
                WSAGetLastError());
        mcp_closesock(s);
        return 0;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 固定。設定可能にしない */
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s, MCP_LISTEN_BACKLOG) != 0) {
        int err = WSAGetLastError();
        if (err == WSAEADDRINUSE) {
            /* TCPのbind失敗は任意のプログラムとの衝突でも起きる。AF_UNIX版と違い
             * ソケットファイルをprobeして相手を特定できないので「ix68kの多重起動」
             * と断定しない。 */
            pal_log("MCP: 127.0.0.1:%u を bind できません。ポートが既に使用中です\n",
                    (unsigned)port);
            pal_log("  ix68k の多重起動か、他のプログラムとの衝突が考えられます。\n");
            pal_log("  複数の ix68k を同時に動かす場合は、環境変数 IX68K_MCP_PORT に"
                    "別のポート番号を指定してください。\n");
        } else {
            pal_log("MCP: 127.0.0.1:%u の bind/listen に失敗しました (WSAエラー %d)\n",
                    (unsigned)port, err);
        }
        mcp_closesock(s);
        return 0;
    }
    snprintf(desc, desc_size, "127.0.0.1:%u", (unsigned)port);
    *out = s;
    return 1;
#else
    const char *sock = mcp_sock_path();
    if (!sock) {
        pal_log("MCP: IX68K_MCP_SOCK のパスが長すぎます。MCPサーバは起動しません。\n");
        return 0;
    }
    if (mcp_socket_is_live(sock)) {
        pal_log("MCP socket already in use by another ix68k instance: %s\n", sock);
        pal_log("  多重起動の可能性があります。既存プロセスを終了してから起動してください。\n");
        pal_log("  意図的に複数起動したい場合は環境変数 IX68K_MCP_SOCK に別のパスを"
                "指定してください。\n");
        return 0;
    }
    unlink(sock);
    mcp_sock_t s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == MCP_INVALID_SOCK)
        return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(s, MCP_LISTEN_BACKLOG) < 0) {
        pal_log("MCP: %s の bind/listen に失敗しました: %s\n", sock, strerror(errno));
        mcp_closesock(s);
        return 0;
    }
    s_mcp_path_owned = 1;
    snprintf(desc, desc_size, "%s", sock);
    *out = s;
    return 1;
#endif
}

void mcp_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return;
#endif
    char desc[MCP_ENDPOINT_DESC_MAX];
    if (!mcp_open_listener(&s_mcp_srv, desc, sizeof(desc))) {
        s_mcp_srv = MCP_INVALID_SOCK;
        return;
    }
#ifdef _WIN32
    {
        u_long mode = 1;
        ioctlsocket(s_mcp_srv, FIONBIO, &mode);
    }
#else
    fcntl(s_mcp_srv, F_SETFL, O_NONBLOCK);
#endif
    pal_log("MCP endpoint: %s\n", desc);
}

void mcp_cleanup(void) {
    mcp_drop_client();
    if (s_mcp_srv != MCP_INVALID_SOCK) {
        mcp_closesock(s_mcp_srv);
        s_mcp_srv = MCP_INVALID_SOCK;
    }
#ifndef _WIN32
    if (s_mcp_path_owned) {
        unlink(mcp_sock_path());
        s_mcp_path_owned = 0;
    }
#endif
#ifdef _WIN32
    WSACleanup();
#endif
}

/* 受信済み1フレーム分の payload を処理する。payload は破壊してよい
 * （ヘッダ行の終端 '\n' を '\0' に書き換えて文字列として扱う）。 */
static void mcp_handle_frame(x68k_bus_t *bus, uint8_t *payload, uint32_t payload_len) {
    uint8_t *nl = memchr(payload, '\n', payload_len);
    if (!nl) {
        s_mcp_reply_tag[0] = '\0';
        mcp_reply("ERR BADARG malformed frame (no header line)\n", 46);
        return;
    }
    *nl = '\0';
    char *header = (char *)payload;
    uint8_t *body = nl + 1;
    uint32_t body_len = payload_len - (uint32_t)(nl - payload) - 1;

    char *sp = strchr(header, ' ');
    if (!sp) {
        s_mcp_reply_tag[0] = '\0';
        mcp_reply("ERR BADARG malformed frame (missing group)\n", 44);
        return;
    }
    size_t taglen = (size_t)(sp - header);
    if (taglen == 0 || taglen >= sizeof(s_mcp_reply_tag)) {
        s_mcp_reply_tag[0] = '\0';
        mcp_reply("ERR BADARG malformed frame (bad tag)\n", 38);
        return;
    }
    memcpy(s_mcp_reply_tag, header, taglen);
    s_mcp_reply_tag[taglen] = '\0';

    s_mcp_context.request_body = body_len ? body : NULL;
    s_mcp_context.request_body_len = body_len;

    if (!mcp_commands_handle(bus, &s_mcp_context, sp + 1)) {
        /* 未知のグループ。エラー応答は "ERR <CODE> <message>" 固定
         * （commands.c の MCP_ERR_* と同じ書式）。 */
        char reply[128];
        int n = snprintf(reply, sizeof(reply), "ERR UNKNOWN no such command group: %.64s\n",
                         sp + 1);
        mcp_reply(reply, (size_t)n);
    }
    s_mcp_context.request_body = NULL;
    s_mcp_context.request_body_len = 0;
}

void mcp_tick(x68k_bus_t *bus) {
    if (s_mcp_srv == MCP_INVALID_SOCK)
        return;
    s_mcp_send_deadline_valid = 0;
    if (s_mcp_cli == MCP_INVALID_SOCK) {
        s_mcp_cli = accept(s_mcp_srv, NULL, NULL);
        if (s_mcp_cli != MCP_INVALID_SOCK) {
#ifdef _WIN32
            u_long mode = 1;
            ioctlsocket(s_mcp_cli, FIONBIO, &mode);
            /* mcp_reply()は1応答を最大4回のsend()に分けており、AF_UNIXには無い
             * NagleアルゴリズムがTCPでは遅延ACKと噛み合って応答が数十〜200ms
             * 遅れ得る。性能上の設定なので、失敗しても
             * fatalにせず警告に留めて接続は受理する（安全性の前提である
             * SO_EXCLUSIVEADDRUSEとは対照的な扱い）。 */
            BOOL nodelay = TRUE;
            if (setsockopt(s_mcp_cli, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay,
                           sizeof(nodelay)) != 0) {
                pal_log("MCP: TCP_NODELAY を設定できません（WSA error %d）。"
                        "応答が遅くなる可能性がありますが動作は継続します。\n",
                        WSAGetLastError());
            }
#else
            fcntl(s_mcp_cli, F_SETFL, O_NONBLOCK);
#endif
            mcp_rx_reset();
        }
    }
    if (s_mcp_cli != MCP_INVALID_SOCK) {
        uint8_t tmp[256];
        ssize_t n;
        while ((n = recv(s_mcp_cli, tmp, sizeof(tmp), 0)) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                uint8_t b = tmp[i];
                if (!s_mcp_rx_in_payload) {
                    s_mcp_rx_lenbuf[s_mcp_rx_lenbuf_have++] = b;
                    if (s_mcp_rx_lenbuf_have < 4)
                        continue;
                    s_mcp_rx_lenbuf_have = 0;
                    s_mcp_rx_frame_len = ((uint32_t)s_mcp_rx_lenbuf[0] << 24) |
                                         ((uint32_t)s_mcp_rx_lenbuf[1] << 16) |
                                         ((uint32_t)s_mcp_rx_lenbuf[2] << 8) |
                                         (uint32_t)s_mcp_rx_lenbuf[3];
                    if (s_mcp_rx_frame_len == 0 || s_mcp_rx_frame_len > MCP_FRAME_MAX) {
                        /* フレーム長が信頼できない状態で読み捨てて再同期を試みる
                         * より、繋ぎ直させる方が単純（旧クライアントの誤接続も
                         * ここで弾かれる）。 */
                        s_mcp_reply_tag[0] = '\0';
                        mcp_reply("ERR BADARG bad frame length\n", 29);
                        mcp_drop_client();
                        goto after_recv;
                    }
                    s_mcp_rx_have = 0;
                    s_mcp_rx_in_payload = 1;
                } else {
                    s_mcp_rx_payload[s_mcp_rx_have++] = b;
                    if (s_mcp_rx_have < s_mcp_rx_frame_len)
                        continue;
                    s_mcp_rx_in_payload = 0;
                    mcp_handle_frame(bus, s_mcp_rx_payload, s_mcp_rx_frame_len);
                    /* 応答の送信失敗で mcp_drop_client() 済みなら、残りのバイトは
                     * 死んだクライアント宛なので実行せず、受信ループも抜ける。 */
                    if (s_mcp_cli == MCP_INVALID_SOCK)
                        goto after_recv;
                }
            }
        }
    after_recv:
        /* n==0 は相手の正常切断。n<0 はエラーだが、EAGAIN/EWOULDBLOCK/EINTR は
         * 「今読めるデータが無いだけ」で正常（非ブロッキングソケットの通常運用）。
         * それ以外（ECONNRESET等）を無視すると壊れたソケットが s_mcp_cli に
         * 残り続け、以後 accept() が永久に呼ばれなくなる（M-4）。
         * 送信失敗での drop 後は上の goto で来るためここは偽になるが、
         * 二重 drop を避ける防御として残す。 */
        if (s_mcp_cli != MCP_INVALID_SOCK && (n == 0 || (n < 0 && !mcp_io_would_block())))
            mcp_drop_client();
    }
    mcp_commands_poll(bus, &s_mcp_context);
}
