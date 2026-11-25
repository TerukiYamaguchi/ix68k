/*
 * システムポート#6 ($E8E00F, Power OFF Control) の単体テスト。
 * 前面スイッチOFF中に $00,$0F,$0F をバイト書込みすると電源断要求が立つこと、
 * 順序崩れ・スイッチON・リセットの扱いを遷移表に沿って検証する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

#define SW_ON  1
#define SW_OFF 0

static x68k_bus_t *new_bus(int sw_on) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_mfp_power_switch(b, sw_on);
    return b;
}

static void wr8(mc68k_bus_t *h, uint32_t addr, uint8_t val) {
    bool ok = h->write_byte(h->ctx, addr, val);
    assert(ok);
}

/* スイッチOFFの新しい bus に seq を順に書き、要求の有無を返す。 */
static int run_seq(const uint8_t *seq, int n) {
    x68k_bus_t *b = new_bus(SW_OFF);
    mc68k_bus_t h = x68k_bus_get_hooks(b);
    for (int i = 0; i < n; i++)
        wr8(&h, SYSPORT6_ADDR, seq[i]);
    int req = x68k_bus_power_off_requested(b);
    x68k_bus_destroy(b);
    return req;
}

#define EXPECT_SEQ(expected, ...)                         \
    do {                                                  \
        const uint8_t s[] = {__VA_ARGS__};                \
        assert(run_seq(s, (int)sizeof(s)) == (expected)); \
    } while (0)

int main(void) {
    /* 6. create 直後は要求なし。 */
    {
        x68k_bus_t *b = x68k_bus_create();
        assert(b != NULL);
        assert(!x68k_bus_power_off_requested(b));
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ0);
        x68k_bus_destroy(b);
    }

    /* 1. スイッチOFFで正順 → 要求が立ち、ステートは待機へ戻る。 */
    {
        x68k_bus_t *b = new_bus(SW_OFF);
        mc68k_bus_t h = x68k_bus_get_hooks(b);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ1);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ2);
        assert(!x68k_bus_power_off_requested(b));
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(x68k_bus_power_off_requested(b));
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ0);
        x68k_bus_destroy(b);
    }

    /* 2. スイッチONでは正順でも要求が立たず、ステートは常に待機。 */
    {
        x68k_bus_t *b = new_bus(SW_ON);
        mc68k_bus_t h = x68k_bus_get_hooks(b);
        const uint8_t s[] = {0x00, 0x0F, 0x0F};
        for (unsigned i = 0; i < sizeof(s); i++) {
            wr8(&h, SYSPORT6_ADDR, s[i]);
            assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ0);
        }
        assert(!x68k_bus_power_off_requested(b));
        x68k_bus_destroy(b);
    }

    /* 3. スイッチOFFでの各種書込み列。 */
    EXPECT_SEQ(0, 0x0F, 0x0F, 0x00);
    EXPECT_SEQ(0, 0x00, 0x0F, 0x00);
    EXPECT_SEQ(1, 0x00, 0x0F, 0x00, 0x0F, 0x0F);
    EXPECT_SEQ(1, 0x00, 0x00, 0x0F, 0x0F);
    EXPECT_SEQ(0, 0x00, 0xFF, 0x0F, 0x0F);
    EXPECT_SEQ(1, 0x00, 0x0F, 0x0F, 0xFF); /* 要求は立った後に $FF を書いても残る */
    {
        x68k_bus_t *b = new_bus(SW_OFF);
        mc68k_bus_t h = x68k_bus_get_hooks(b);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ1);
        x68k_bus_destroy(b);
    }

    /* 4. シーケンス途中のスイッチON/OFF。 */
    {
        x68k_bus_t *b = new_bus(SW_OFF);
        mc68k_bus_t h = x68k_bus_get_hooks(b);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ2);
        x68k_bus_mfp_power_switch(b, SW_ON);
        wr8(&h, SYSPORT6_ADDR, 0x0F); /* ON中の書込みでステートが待機へ戻る */
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ0);
        x68k_bus_mfp_power_switch(b, SW_OFF);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(!x68k_bus_power_off_requested(b));
        x68k_bus_destroy(b);

        /* ON中に $00、OFFへ戻して $0F,$0F → 立たない。 */
        b = new_bus(SW_ON);
        h = x68k_bus_get_hooks(b);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        x68k_bus_mfp_power_switch(b, SW_OFF);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(!x68k_bus_power_off_requested(b));
        x68k_bus_destroy(b);

        /* OFFで $00 後、ONにして書込みなしでOFFへ戻し $0F,$0F → 立つ。 */
        b = new_bus(SW_OFF);
        h = x68k_bus_get_hooks(b);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        x68k_bus_mfp_power_switch(b, SW_ON);
        x68k_bus_mfp_power_switch(b, SW_OFF);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(x68k_bus_power_off_requested(b));
        x68k_bus_destroy(b);
    }

    /* 5. 公開版リセットで途中ステートは待機へ戻り、立った要求は残る。 */
    {
        x68k_bus_t *b = new_bus(SW_OFF);
        mc68k_bus_t h = x68k_bus_get_hooks(b);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        x68k_bus_reset_peripheral(b);
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ0);
        assert(!x68k_bus_power_off_requested(b));
        wr8(&h, SYSPORT6_ADDR, 0x00);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(x68k_bus_power_off_requested(b));
        x68k_bus_reset_peripheral(b);
        assert(x68k_bus_power_off_requested(b));
        x68k_bus_destroy(b);
    }

    /* 7. ワード/ロング書込み（CPU 経路）。 */
    {
        x68k_bus_t *b = new_bus(SW_OFF);
        mc68k_bus_t h = x68k_bus_get_hooks(b);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        bool ok = h.write_word(h.ctx, 0xE8E00Eu, 0x000Fu); /* 下位バイトが $E8E00F */
        assert(ok);
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ2);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(x68k_bus_power_off_requested(b));
        x68k_bus_destroy(b);

        b = new_bus(SW_OFF);
        h = x68k_bus_get_hooks(b);
        wr8(&h, SYSPORT6_ADDR, 0x00);
        ok = h.write_long(h.ctx, 0xE8E00Cu, 0x0000000Fu); /* $E8E00F への1回 */
        assert(ok);
        assert(b->poweroff_seq == SYSPORT6_POWEROFF_SEQ2);
        wr8(&h, SYSPORT6_ADDR, 0x0F);
        assert(x68k_bus_power_off_requested(b));
        x68k_bus_destroy(b);

        /* 裏口 API（補助）。 */
        b = new_bus(SW_OFF);
        x68k_bus_write_byte(b, SYSPORT6_ADDR, 0x00);
        x68k_bus_write16(b, 0xE8E00Eu, 0x000Fu);
        x68k_bus_write_byte(b, SYSPORT6_ADDR, 0x0F);
        assert(x68k_bus_power_off_requested(b));
        x68k_bus_destroy(b);
    }

    /* 8. $E8E00F の読み戻し（crtc_regs[] 互換）。 */
    {
        x68k_bus_t *b = new_bus(SW_OFF);
        mc68k_bus_t h = x68k_bus_get_hooks(b);
        uint8_t v = 0;
        wr8(&h, SYSPORT6_ADDR, 0x5A);
        bool ok = h.read_byte(h.ctx, SYSPORT6_ADDR, &v);
        assert(ok);
        assert(v == 0x5A);
        x68k_bus_destroy(b);
    }

    printf("sysport_poweroff_test: OK\n");
    return 0;
}
