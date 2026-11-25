/*
 * MCPサーバ（`server.c`）の実ソケット結合テスト。
 *
 * 外部デバッガは1接続へ同期frameを複数本続けて送るpipelineを使う。`mcp_commands_test`は
 * `server.c`を迂回して応答文字列だけを見るので、フレーミング・tag・切断復旧は
 * ここで実ソケットを通して確かめる。同一プロセスの非ブロッキングclientを
 * listenerへ繋ぎ、`mcp_tick()`とclient側のreadを同じループで回す。サーバの送信
 * 締切（5秒）には依存せず、数百msのテスト側期限で失敗させる。
 *
 * アプリ層（`s_app`・`app_*`・`x68k_cycle_catchup`）とレンダラ一式を要する
 * `mcp_screenshot_make_ppm()`は`mcp_commands_test`と同じくスタブする。
 * 本テストはPOSIXのAF_UNIXソケットを使う（Windowsは別経路のため対象外）。
 */
#include "app_internal.h"
#include "bus/x68k_bus.h"
#include "mcp/internal.h"
#include "platform/platform.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define TEST_DEADLINE_US 500000ull
#define TEST_FRAME_MAX   4096
#define TEST_SOCK_FMT    "/tmp/ix68k_pipeline_test_%d.sock"
#define TEST_BYE_ROUNDS  20
/* 応答を読まないclientへ送り続けるHELLOの本数。応答総量がソケットバッファを
 * 十分超える数にして、サーバの送信が途中で詰まるようにする。 */
#define TEST_FLOOD_FRAMES   6000
/* サーバが詰まっている間に別スレッドがclientを閉じるまでの待ち。 */
#define TEST_CLOSER_DELAY_MS 100
#define TEST_QUIET_ROUNDS    50
/* 送信失敗後に実行されないことを確かめるMEM WRITEの書き込み先。 */
#define TEST_WRITE_ADDR      0x2000
/* 先行clientが処理中の間に、応答を待たず閉じる後続接続の本数（旧backlog 4を十分超える）。 */
#define TEST_BACKLOG_CLIENTS 40

/* ---- アプリ層スタブ ---------------------------------------------------- */

x68k_app_t s_app = {.native_w = 512, .native_h = 512};

void app_reset_machine(x68k_bus_t *bus) {
    (void)bus;
}
void app_force_fd0_boot_begin(x68k_bus_t *bus) {
    (void)bus;
}
void app_force_fd0_boot_cancel(x68k_bus_t *bus) {
    (void)bus;
}
void x68k_cycle_catchup(x68k_bus_t *bus, int ran) {
    (void)bus;
    (void)ran;
}
void iocs_key_wait_input_queued(void) {
    s_app.key_input_waiting = 0;
}
void iocs_key_wait_poll(x68k_bus_t *bus) {
    (void)bus;
}
uint8_t *mcp_screenshot_make_ppm(x68k_bus_t *bus, int mask, int *out_len) {
    (void)bus;
    (void)mask;
    (void)out_len;
    return NULL;
}

/* ---- client側ヘルパ ---------------------------------------------------- */

static x68k_bus_t *s_bus;
static char s_sock_path[108];

/* clientの受信状態。frame境界をまたぐ残りバイトを呼び出し間で持ち越し、
 * 余剰応答（二重返信）を取りこぼさないようにする。 */
typedef struct {
    int fd;
    uint8_t buf[TEST_FRAME_MAX * 4];
    size_t have;
} client_t;

typedef struct {
    char text[TEST_FRAME_MAX];
} reply_t;

static void client_connect(client_t *c) {
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(c->fd >= 0);
    c->have = 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s_sock_path);
    /* 非ブロッキングでconnectする。受付待ち行列が溢れた場合、Linuxのブロッキングconnectは
     * 失敗せず待ち続けてテストがハングするため、EAGAIN/ECONNREFUSEDで即失敗させる。 */
    assert(fcntl(c->fd, F_SETFL, O_NONBLOCK) == 0);
    assert(connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
}

static void ticks(int rounds) {
    for (int i = 0; i < rounds; i++)
        mcp_tick(s_bus);
}

/* client を閉じ、サーバが切断を拾うまでtickを回す。 */
static void client_close(client_t *c) {
    close(c->fd);
    c->fd = -1;
    ticks(TEST_BYE_ROUNDS);
}

/* "<tag> <line>\n" を1frame（<uint32 BE len><payload>）として組み立てる。 */
static size_t build_frame(uint8_t *out, const char *tag, const char *line) {
    char payload[256];
    int n = snprintf(payload, sizeof(payload), "%s %s\n", tag, line);
    assert(n > 0 && (size_t)n < sizeof(payload));
    out[0] = (uint8_t)((unsigned)n >> 24);
    out[1] = (uint8_t)((unsigned)n >> 16);
    out[2] = (uint8_t)((unsigned)n >> 8);
    out[3] = (uint8_t)n;
    memcpy(out + 4, payload, (size_t)n);
    return (size_t)n + 4;
}

static void send_all(client_t *c, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(c->fd, data + sent, len - sent, 0);
        assert(n > 0);
        sent += (size_t)n;
    }
}

/* bufに溜まった完結frameを1本取り出す。無ければ0。 */
static int pop_frame(client_t *c, reply_t *out) {
    if (c->have < 4)
        return 0;
    uint32_t len = ((uint32_t)c->buf[0] << 24) | ((uint32_t)c->buf[1] << 16) |
                   ((uint32_t)c->buf[2] << 8) | (uint32_t)c->buf[3];
    assert(len > 0 && len < sizeof(out->text));
    if (c->have < 4 + (size_t)len)
        return 0;
    memcpy(out->text, c->buf + 4, len);
    out->text[len] = '\0';
    c->have -= 4 + (size_t)len;
    memmove(c->buf, c->buf + 4 + len, c->have);
    return 1;
}

/* mcp_tick()を回しながらclientのreadをdrainし、完結したframeを want 本集める。
 * 期限までに揃わなければ集まった本数を返す（呼び出し側がassertする）。
 * 余りバイトは c->buf に残り、次の呼び出しで先に消費される。 */
static int collect_frames(client_t *c, reply_t *out, int want) {
    int got = 0;
    uint64_t deadline = pal_get_ticks_us() + TEST_DEADLINE_US;
    while (got < want) {
        while (got < want && pop_frame(c, &out[got]))
            got++;
        if (got == want || pal_get_ticks_us() >= deadline)
            break;
        mcp_tick(s_bus);
        ssize_t n = recv(c->fd, c->buf + c->have, sizeof(c->buf) - c->have, 0);
        if (n > 0)
            c->have += (size_t)n;
    }
    return got;
}

/* 余剰応答（二重返信）が無いこと。期待数を集めた後もtickを回し続けて読み切り、
 * 未消費バイトもrecvの新着も無いことを確認する。 */
static void assert_no_extra_reply(client_t *c) {
    for (int i = 0; i < TEST_QUIET_ROUNDS; i++) {
        mcp_tick(s_bus);
        ssize_t n = recv(c->fd, c->buf + c->have, sizeof(c->buf) - c->have, 0);
        if (n > 0)
            c->have += (size_t)n;
        usleep(1000);
    }
    assert(c->have == 0);
}

/* 次の接続が正常に動くこと（HELLOが1応答だけ返る）を確認する。 */
static void assert_server_recovered(const char *tag) {
    client_t c;
    client_connect(&c);
    uint8_t frame[300];
    send_all(&c, frame, build_frame(frame, tag, "HELLO"));
    reply_t r[1];
    assert(collect_frames(&c, r, 1) == 1);
    size_t taglen = strlen(tag);
    assert(strncmp(r[0].text, tag, taglen) == 0);
    assert(strncmp(r[0].text + taglen, " OK PROTO=5 BUILD=", 18) == 0);
    assert_no_extra_reply(&c);
    client_close(&c);
}

/* ---- テスト本体 -------------------------------------------------------- */

/* 同期frame2本を1回のsendで送り、tagごとに一応答が送信順に返る。 */
static void test_two_frames_each_get_one_reply(void) {
    client_t c;
    client_connect(&c);
    uint8_t buf[600];
    size_t len = build_frame(buf, "p1", "HELLO");
    len += build_frame(buf + len, "p2", "NO_SUCH_GROUP X");
    send_all(&c, buf, len);

    reply_t r[2];
    assert(collect_frames(&c, r, 2) == 2);
    assert(strncmp(r[0].text, "p1 OK PROTO=5 BUILD=", 20) == 0);
    assert(strncmp(r[1].text, "p2 ERR UNKNOWN ", 15) == 0);
    assert_no_extra_reply(&c);
    client_close(&c);
}

/* frameが分割到着しても、tick間で再構成して一応答を返す。分割位置は
 * 長さプレフィックスの途中（2バイト目）とpayloadの途中の2通り。 */
static void test_frame_split_across_ticks(void) {
    static const size_t splits[] = {2, 7};
    for (size_t k = 0; k < sizeof(splits) / sizeof(splits[0]); k++) {
        client_t c;
        client_connect(&c);
        uint8_t frame[300];
        size_t len = build_frame(frame, "s1", "HELLO");
        assert(splits[k] < len);
        send_all(&c, frame, splits[k]);
        ticks(TEST_BYE_ROUNDS);
        reply_t r[1];
        /* 前半だけでは応答しない（短い期限で0本）。 */
        for (int i = 0; i < TEST_QUIET_ROUNDS; i++) {
            mcp_tick(s_bus);
            assert(recv(c.fd, c.buf, sizeof(c.buf), 0) < 0);
        }
        send_all(&c, frame + splits[k], len - splits[k]);
        assert(collect_frames(&c, r, 1) == 1);
        assert(strncmp(r[0].text, "s1 OK PROTO=5 BUILD=", 20) == 0);
        assert_no_extra_reply(&c);
        client_close(&c);
    }
}

/* 応答を読まずに切断（サーバは存在しない相手へ応答を書く）しても、次接続が動く。 */
static void test_disconnect_before_reply_then_reconnect(void) {
    client_t c;
    client_connect(&c);
    uint8_t buf[600];
    size_t len = build_frame(buf, "d1", "HELLO");
    len += build_frame(buf + len, "d2", "HELLO");
    send_all(&c, buf, len);
    client_close(&c);
    assert_server_recovered("r1");
}

/* リクエストframeの途中で切断しても、断片が次接続の先頭へ連結されない。 */
static void test_disconnect_mid_request_then_reconnect(void) {
    client_t c;
    client_connect(&c);
    uint8_t frame[300];
    size_t len = build_frame(frame, "m1", "HELLO");
    send_all(&c, frame, len - 3);
    ticks(TEST_BYE_ROUNDS);
    client_close(&c);
    assert_server_recovered("r2");
}

/* 1本目の応答だけ読んだ時点で切断しても、次接続が動く。 */
static void test_disconnect_between_replies_then_reconnect(void) {
    client_t c;
    client_connect(&c);
    uint8_t buf[600];
    size_t len = build_frame(buf, "b1", "HELLO");
    len += build_frame(buf + len, "b2", "HELLO");
    send_all(&c, buf, len);
    reply_t r[1];
    assert(collect_frames(&c, r, 1) == 1);
    assert(strncmp(r[0].text, "b1 OK PROTO=5 BUILD=", 20) == 0);
    client_close(&c);
    assert_server_recovered("r3");
}

/* 別スレッドが一定時間後にclientを閉じる。サーバがmcp_tick()内の送信で
 * 詰まっている間はテスト本体が動けないため、スレッドで切断を起こす。 */
static void *closer_thread(void *arg) {
    int fd = *(int *)arg;
    usleep(TEST_CLOSER_DELAY_MS * 1000);
    close(fd);
    return NULL;
}

/* 応答を読まないclientへHELLOを大量に送り、サーバの応答送信がバッファ満杯で
 * 詰まっている間にclientが切断しても、次接続が動く。
 * 通る経路: mcp_send_all()がpoll()で書き込み可能を待つ→相手のcloseで起こされる
 * （POLLHUP）→send()がEPIPEで失敗→接続を切る。切れる位置は応答と応答の境界
 * （次の長さプレフィックスの送信時、ログ「0/4 バイト」）であり、1応答の内部
 * （0 < sent < len）ではない。応答の途中で切れる経路を通すには応答本体の大きい
 * コマンド（MEM READ等）を使う必要があるが、本テストでは対象外。
 * 送信バッファが応答総量より大きい環境では詰まらず、切断経路を通らないまま
 * 成立する（macOSで経路を通ることを確認）。 */
static void test_disconnect_during_reply_send_then_reconnect(void) {
    client_t c;
    client_connect(&c);
    pthread_t th;
    assert(pthread_create(&th, NULL, closer_thread, &c.fd) == 0);
    uint8_t frame[300];
    size_t len = build_frame(frame, "f1", "HELLO");
    uint64_t deadline = pal_get_ticks_us() + 5 * TEST_DEADLINE_US;
    for (int i = 0; i < TEST_FLOOD_FRAMES && pal_get_ticks_us() < deadline; i++) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(c.fd, frame + sent, len - sent, 0);
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                goto flood_done; /* 切断済み */
            mcp_tick(s_bus);
        }
    }
flood_done:
    assert(pthread_join(th, NULL) == 0);
    c.fd = -1; /* スレッド側でclose済み */
    ticks(TEST_BYE_ROUNDS);
    assert_server_recovered("r4");
}

/* 送信失敗でclientを切り離したあと、同じrecvで読んだ後続frameを実行しない。
 * HELLOとMEM WRITEを1回のsendで送り、tickを回す前に閉じる。サーバはHELLOの応答で
 * 送信に失敗して切り離す（EPIPE相当）ので、後続のMEM WRITEは実行されずメモリが変わらない。
 * 2frameが別々のrecvに分かれた場合は修正前でも通り得るが、修正後に誤って失敗はしない。 */
static void test_send_failure_skips_following_frame(void) {
    client_t c;
    client_connect(&c);
    uint8_t buf[600];
    size_t len = build_frame(buf, "w1", "HELLO");
    /* 本文を'Z'1バイトにするため、build_frame()（末尾に\nを足す）は使わず手で組む。 */
    static const char write_payload[] = "w2 MEM WRITE 2000\nZ";
    uint32_t pl = (uint32_t)(sizeof(write_payload) - 1);
    buf[len++] = (uint8_t)(pl >> 24);
    buf[len++] = (uint8_t)(pl >> 16);
    buf[len++] = (uint8_t)(pl >> 8);
    buf[len++] = (uint8_t)pl;
    memcpy(buf + len, write_payload, pl);
    len += pl;
    uint8_t before = x68k_bus_peek8(s_bus, TEST_WRITE_ADDR);
    assert(before != 'Z');
    send_all(&c, buf, len);
    client_close(&c); /* 内部でtickを回し、サーバに切断を処理させる */
    assert(x68k_bus_peek8(s_bus, TEST_WRITE_ADDR) == before);
    assert_server_recovered("r5");
}

/* 1本目のclientが接続を占有している間に後続接続が短時間に何本も来ても、接続を
 * 拒否されず、占有が解けた後に正常動作へ戻る。外部デバッガは
 * DBG WAITで塞がれている間、タイムアウトで諦めた接続を繰り返し張る。 */
static void test_many_queued_connections_not_refused(void) {
    client_t holder;
    client_connect(&holder);
    ticks(TEST_BYE_ROUNDS); /* holderをaccept済みにする。以降のtickは受付されない */
    for (int i = 0; i < TEST_BACKLOG_CLIENTS; i++) {
        client_t q;
        client_connect(&q); /* backlog溢れならここでassert失敗 */
        uint8_t frame[300];
        send_all(&q, frame, build_frame(frame, "q", "HELLO"));
        close(q.fd);
    }
    client_close(&holder);
    ticks(TEST_BACKLOG_CLIENTS * 2); /* 待っていた接続を順に捌かせる */
    assert_server_recovered("r6");
}

int main(void) {
    /* 本番はmain.cがSIGPIPEを無視する。相手切断済みへの応答でテストが死なないよう揃える。 */
    signal(SIGPIPE, SIG_IGN);
    snprintf(s_sock_path, sizeof(s_sock_path), TEST_SOCK_FMT, (int)getpid());
    assert(setenv("IX68K_MCP_SOCK", s_sock_path, 1) == 0);
    s_bus = x68k_bus_create();
    assert(s_bus != NULL);
    mcp_init();

    test_two_frames_each_get_one_reply();
    test_frame_split_across_ticks();
    test_disconnect_before_reply_then_reconnect();
    test_disconnect_mid_request_then_reconnect();
    test_disconnect_between_replies_then_reconnect();
    test_disconnect_during_reply_send_then_reconnect();
    test_send_failure_skips_following_frame();
    test_many_queued_connections_not_refused();

    mcp_cleanup();
    x68k_bus_destroy(s_bus);
    printf("mcp_server_pipeline_test: all passed\n");
    return 0;
}
