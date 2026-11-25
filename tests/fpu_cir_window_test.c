/*
 * FPU-CIR（CZ-6BP1/CZ-6BP1A）の配置・境界テスト。
 *
 * ボード無効/primary/secondary で、CPU バスコールバックが window だけをデコードし、
 * 選択外の XB1 をバスエラーにすること、MCP 等の裏口（x68k_bus_read8/peek8）が
 * バスエラーを通知せず window 内を全1で返すことを確認する。CIR の中身は別テストで扱う。
 */
#include "bus/bus_internal.h"
#include "bus/x68k_bus.h"
#include <assert.h>
#include <stdio.h>

#define XB1_START        0xE9E000u
#define XB1_LAST         0xE9FFFFu
#define PRIMARY_BASE     0xE9E000u
#define SECONDARY_BASE   0xE9E080u
#define WINDOW_SIZE      0x20u
#define BYTE_ALL_ONES    0xFFu
#define WORD_ALL_ONES    0xFFFFu
#define LONG_ALL_ONES    0xFFFFFFFFu
#define WINDOW_LAST_WORD 0x1Eu

static bool cpu_read_ok(mc68k_bus_t *h, uint32_t addr) {
    uint8_t v;
    return h->read_byte(h->ctx, addr, &v);
}

static bool cpu_write_ok(mc68k_bus_t *h, uint32_t addr) {
    return h->write_byte(h->ctx, addr, 0x55u);
}

/* window [base, base+WINDOW_SIZE) だけが通り、XB1 の残りは全てバスエラーであること */
static void check_decode(x68k_bus_t *b, bool enabled, uint32_t base) {
    mc68k_bus_t h = x68k_bus_get_hooks(b);
    for (uint32_t addr = XB1_START; addr <= XB1_LAST; addr++) {
        bool in_window = enabled && addr >= base && addr < base + WINDOW_SIZE;
        assert(cpu_read_ok(&h, addr) == in_window);
        assert(cpu_write_ok(&h, addr) == in_window);
        if (in_window) {
            uint8_t v = 0;
            assert(h.read_byte(h.ctx, addr, &v) && v == BYTE_ALL_ONES);
            /* 裏口はバスエラーを持たず、有効 window は全1、それ以外は従来の0x00 */
            assert(x68k_bus_read8(b, addr) == BYTE_ALL_ONES);
            assert(x68k_bus_peek8(b, addr) == BYTE_ALL_ONES);
            /* 裏口の書込みは無視され、読出しは全1のまま */
            x68k_bus_write8(b, addr, 0x00u);
            assert(x68k_bus_read8(b, addr) == BYTE_ALL_ONES);
        } else {
            assert(x68k_bus_read8(b, addr) == 0x00u);
            assert(x68k_bus_peek8(b, addr) == 0x00u);
        }
    }
    /* XB1 の両隣は従来どおり: IOI($E9DFFF)は実在、DEV($EA0000)は拡張スロットの gap */
    assert(cpu_read_ok(&h, XB1_START - 1u));
    assert(!cpu_read_ok(&h, XB1_LAST + 1u));
}

/* word/long の境界: CIR の中身は fpu_cir_test.c。ここでは window 端のバスエラー位置だけ見る */
static void check_word_long(x68k_bus_t *b, uint32_t base) {
    mc68k_bus_t h = x68k_bus_get_hooks(b);
    uint16_t w = 0;
    uint32_t l = 0;
    /* 予約位置(+$08)の word/long は全1、write は無視で完了する */
    assert(h.read_word(h.ctx, base + 0x08u, &w) && w == WORD_ALL_ONES);
    assert(h.write_word(h.ctx, base + 0x08u, 0x1234u));
    assert(h.read_long(h.ctx, base + 0x08u, &l) && l == LONG_ALL_ONES);
    /* +$1E の long は後半が window 外（+$20）に出るのでバスエラー（前半は完了済み） */
    assert(!h.read_long(h.ctx, base + WINDOW_LAST_WORD, &l));
    assert(!h.write_long(h.ctx, base + WINDOW_LAST_WORD, 0u));
    /* window 直後の word もバスエラー */
    assert(!h.read_word(h.ctx, base + WINDOW_SIZE, &w));
}

int main(void) {
    /* 無効（既定）: XB1 全域がバスエラー */
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    check_decode(b, false, PRIMARY_BASE);
    assert(x68k_bus_set_fpu_cir(b, 0, X68K_FPU_CIR_PRIMARY)); /* enable=0 は no-op */
    check_decode(b, false, PRIMARY_BASE);
    x68k_bus_destroy(b);
    printf("fpu_cir_window_test: disabled OK\n");

    /* モデル生成失敗（NULL）: false を返し、primary/secondary どちらでも無効のまま */
    b = x68k_bus_create();
    assert(b != NULL);
    assert(!x68k_bus_attach_fpu_cir(b, NULL, X68K_FPU_CIR_PRIMARY));
    check_decode(b, false, PRIMARY_BASE);
    assert(!x68k_bus_attach_fpu_cir(b, NULL, X68K_FPU_CIR_SECONDARY));
    check_decode(b, false, SECONDARY_BASE);
    x68k_bus_destroy(b);
    printf("fpu_cir_window_test: create failure OK\n");

    b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_set_fpu_cir(b, 1, X68K_FPU_CIR_PRIMARY));
    check_decode(b, true, PRIMARY_BASE);
    check_word_long(b, PRIMARY_BASE);
    x68k_bus_destroy(b);
    printf("fpu_cir_window_test: primary OK\n");

    b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_set_fpu_cir(b, 1, X68K_FPU_CIR_SECONDARY));
    check_decode(b, true, SECONDARY_BASE);
    check_word_long(b, SECONDARY_BASE);
    x68k_bus_destroy(b);
    printf("fpu_cir_window_test: secondary OK\n");

    printf("fpu_cir_window_test: ALL OK\n");
    return 0;
}
