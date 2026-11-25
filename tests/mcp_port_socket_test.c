/* Windows専用の実ソケット試験。src/mcp/server.c の
 * Windows版 mcp_open_listener() と同じ手順（AF_INET / 127.0.0.1固定 /
 * SO_EXCLUSIVEADDRUSE）をここで再現して検証する。mcp_open_listener() は static かつ
 * pal_log 依存でテストから素で呼べないため、本体と試験でコードが二重になる
 * （既知の弱点）。
 *
 * src/mcp/server.c はリンクしない（pal_log・SDL・busを引き込むため）。 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* mcp/port.h は使わないが、CMakeターゲットがsrc/mcp/port.cをリンクしている
 * （Windows専用機能をまとめて確認する趣旨のターゲットのため）。 */
#include "mcp/port.h"

static SOCKET open_loopback_listener(uint16_t port, int exclusive) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s != INVALID_SOCKET);
    if (exclusive) {
        BOOL v = TRUE;
        int rc = setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&v, sizeof(v));
        assert(rc == 0);
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 固定。D2と同じ */
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (listen(s, 4) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static uint16_t get_bound_port(SOCKET s) {
    struct sockaddr_in addr;
    int len = sizeof(addr);
    int rc = getsockname(s, (struct sockaddr *)&addr, &len);
    assert(rc == 0);
    return ntohs(addr.sin_port);
}

/* a: loopback限定でlistenしている（D2）。 */
static void test_loopback_only(void) {
    SOCKET s = open_loopback_listener(0, 1); /* port=0でOSに空きポートを選ばせる */
    assert(s != INVALID_SOCKET);
    struct sockaddr_in addr;
    int len = sizeof(addr);
    int rc = getsockname(s, (struct sockaddr *)&addr, &len);
    assert(rc == 0);
    assert(ntohl(addr.sin_addr.s_addr) == INADDR_LOOPBACK); /* INADDR_ANYでないこと */
    closesocket(s);
    printf("test_loopback_only: OK\n");
}

/* b: 同一ポートへの二重bindが失敗する（D4、多重起動検出の核心）。 */
static void test_duplicate_bind_fails(void) {
    SOCKET s1 = open_loopback_listener(0, 1);
    assert(s1 != INVALID_SOCKET);
    uint16_t port = get_bound_port(s1);

    SOCKET s2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s2 != INVALID_SOCKET);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    int rc = bind(s2, (struct sockaddr *)&addr, sizeof(addr));
    assert(rc != 0);
    assert(WSAGetLastError() == WSAEADDRINUSE);
    closesocket(s2);
    closesocket(s1);
    printf("test_duplicate_bind_fails: OK\n");
}

/* c: SO_EXCLUSIVEADDRUSEが機能している。SO_REUSEADDRを立てた2つ目でも奪えないこと
 * （D4: SO_REUSEADDRを使う他プロセスの行儀に依存しないことの実証）。 */
static void test_exclusiveaddruse_blocks_reuseaddr(void) {
    SOCKET s1 = open_loopback_listener(0, 1);
    assert(s1 != INVALID_SOCKET);
    uint16_t port = get_bound_port(s1);

    SOCKET s2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(s2 != INVALID_SOCKET);
    BOOL reuse = TRUE;
    setsockopt(s2, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    int rc = bind(s2, (struct sockaddr *)&addr, sizeof(addr));
    assert(rc != 0); /* SO_REUSEADDRを立てても、相手のSO_EXCLUSIVEADDRUSEには勝てない */
    closesocket(s2);
    closesocket(s1);
    printf("test_exclusiveaddruse_blocks_reuseaddr: OK\n");
}

/* d: フレーム分割とバイナリ応答の処理（src/mcp/server.c の mcp_tick() 366-392行の
 * 受信状態機械を再現）。mcp_handle_frame() 相当はstaticでテストから呼べないため、
 * ここではフレーミング（長さプレフィックス→payload）の組み立てだけを検証する
 * （commands.c 依存の分岐は対象外）。 */
#define FRAME_MAX 4096

typedef struct {
    uint8_t payload[FRAME_MAX];
    uint32_t have;
    uint32_t frame_len;
    uint8_t lenbuf[4];
    int lenbuf_have;
    int in_payload;
    int complete;
} frame_state_t;

static void frame_reset(frame_state_t *st) {
    memset(st, 0, sizeof(*st));
}

static void frame_feed(frame_state_t *st, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        if (!st->in_payload) {
            st->lenbuf[st->lenbuf_have++] = b;
            if (st->lenbuf_have < 4)
                continue;
            st->lenbuf_have = 0;
            st->frame_len = ((uint32_t)st->lenbuf[0] << 24) | ((uint32_t)st->lenbuf[1] << 16) |
                             ((uint32_t)st->lenbuf[2] << 8) | (uint32_t)st->lenbuf[3];
            st->have = 0;
            st->in_payload = 1;
        } else {
            st->payload[st->have++] = b;
            if (st->have < st->frame_len)
                continue;
            st->in_payload = 0;
            st->complete = 1;
        }
    }
}

static void test_frame_split_byte_by_byte(void) {
    /* 0x00を含むバイナリpayload（途中で切れないことの検査）。 */
    uint8_t payload[8] = {0x41, 0x00, 0x42, 0x00, 0x00, 0x43, 0x00, 0x44};
    uint32_t plen = sizeof(payload);
    uint8_t frame[4 + sizeof(payload)];
    frame[0] = (uint8_t)(plen >> 24);
    frame[1] = (uint8_t)(plen >> 16);
    frame[2] = (uint8_t)(plen >> 8);
    frame[3] = (uint8_t)plen;
    memcpy(frame + 4, payload, plen);

    frame_state_t st;
    frame_reset(&st);
    for (size_t i = 0; i < sizeof(frame); i++)
        frame_feed(&st, &frame[i], 1); /* 1バイトずつ */
    assert(st.complete);
    assert(st.frame_len == plen);
    assert(memcmp(st.payload, payload, plen) == 0);
    printf("test_frame_split_byte_by_byte: OK\n");
}

static void test_frame_split_arbitrary_chunks(void) {
    uint8_t payload[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    uint32_t plen = sizeof(payload);
    uint8_t frame[4 + sizeof(payload)];
    frame[0] = (uint8_t)(plen >> 24);
    frame[1] = (uint8_t)(plen >> 16);
    frame[2] = (uint8_t)(plen >> 8);
    frame[3] = (uint8_t)plen;
    memcpy(frame + 4, payload, plen);

    /* 長さプレフィックスの途中（3バイト目）と、payloadの途中（先頭+6バイト目）で
     * 分割する — フレーム境界に一切揃っていない分割点を選ぶ。 */
    frame_state_t st;
    frame_reset(&st);
    frame_feed(&st, frame, 3);
    assert(!st.complete);
    frame_feed(&st, frame + 3, 7); /* 残り1バイトの長さ + payload先頭6バイト */
    assert(!st.complete);
    frame_feed(&st, frame + 10, sizeof(frame) - 10);
    assert(st.complete);
    assert(st.frame_len == plen);
    assert(memcmp(st.payload, payload, plen) == 0);
    printf("test_frame_split_arbitrary_chunks: OK\n");
}

int main(void) {
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    assert(rc == 0);

    test_loopback_only();
    test_duplicate_bind_fails();
    test_exclusiveaddruse_blocks_reuseaddr();
    test_frame_split_byte_by_byte();
    test_frame_split_arbitrary_chunks();

    WSACleanup();
    printf("mcp_port_socket_test: OK\n");
    return 0;
}
