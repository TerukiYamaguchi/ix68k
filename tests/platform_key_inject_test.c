/* MCP PASTEの制御文字をX68000キーボードのCtrl+英字として注入する経路のテスト。 */
#include "platform/platform.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint8_t bytes[8];
    int len;
} key_log_t;

static void push_key(void *ctx, uint8_t byte) {
    key_log_t *log = ctx;
    assert(log->len < (int)sizeof(log->bytes));
    log->bytes[log->len++] = byte;
}

static void clear_key(void *ctx) {
    ((key_log_t *)ctx)->len = 0;
}

static void expect_bytes(const key_log_t *log, const uint8_t *expected, int len) {
    assert(log->len == len);
    for (int i = 0; i < len; i++)
        assert(log->bytes[i] == expected[i]);
}

int main(void) {
    key_log_t log = {{0}, 0};
    pal_key_set_callbacks(push_key, clear_key, &log);

    pal_key_inject_ascii(0x1A); /* Ctrl+Z (SUB): COPY CONの終端 */
    const uint8_t ctrl_z[] = {0x71, 0x2A, 0xAA, 0xF1};
    expect_bytes(&log, ctrl_z, 4);

    clear_key(&log);
    pal_key_inject_ascii(0x03); /* Ctrl+C */
    const uint8_t ctrl_c[] = {0x71, 0x2C, 0xAC, 0xF1};
    expect_bytes(&log, ctrl_c, 4);

    clear_key(&log);
    pal_key_inject_ascii(0x09); /* TABは既存の貼り付け意味を維持 */
    const uint8_t tab[] = {0x10, 0x90};
    expect_bytes(&log, tab, 2);

    /* headlessビルドで無言欠落していた記号キー（':' はDIR B:等で使う）。 */
    clear_key(&log);
    pal_key_inject_ascii(':'); /* SHIFT不要（0x28キーのunshift面） */
    const uint8_t colon[] = {0x28, 0xA8};
    expect_bytes(&log, colon, 2);

    clear_key(&log);
    pal_key_inject_ascii('_'); /* SHIFT要（0x34キーのshift面） */
    const uint8_t underscore[] = {0x70, 0x34, 0xB4, 0xF0};
    expect_bytes(&log, underscore, 4);

    printf("platform_key_inject_test: OK\n");
    return 0;
}
