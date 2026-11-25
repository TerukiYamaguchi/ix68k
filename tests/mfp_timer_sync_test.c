/*
 * MFPタイマのチャンク内同期と満了予定算出の単体テスト。
 * mfp_timerb_test.c と同じソース構成・呼び出し流儀を踏襲する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

#define REG_IERB  0xE88009u
#define REG_IMRB  0xE88015u
#define REG_TCDCR 0xE8801Du
#define REG_TCDR  0xE88023u
#define TCDCR_TC_DIV4 0x10u /* TC prescale 1 = ÷4 */

static int s_fake_cycles;

static int fake_cycle_source(void *ctx) {
    (void)ctx;
    return s_fake_cycles;
}

/* Timer C を TCDR/TCDCR で起動し、cpu_hz 上で「予定サイクル-1では満了せず、
 * 予定サイクルちょうどで満了する」ことを確かめる。 */
static void check_expiry_exact(uint32_t cpu_hz, uint8_t tcdr, uint8_t tcdcr) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_set_cpu_hz(b, cpu_hz);
    mfp_reg_write8(b, REG_IERB, 0x20u);
    mfp_reg_write8(b, REG_IMRB, 0x20u);
    mfp_reg_write8(b, REG_TCDR, tcdr);
    mfp_reg_write8(b, REG_TCDCR, tcdcr);

    int to_event = x68k_bus_mfp_cycles_to_next_event(b);
    assert(to_event >= 1 && to_event < INT_MAX);
    if (to_event > 1)
        x68k_bus_mfp_tick(b, to_event - 1);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C));
    assert(x68k_bus_mfp_cycles_to_next_event(b) == 1);
    x68k_bus_mfp_tick(b, 1);
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C));
    x68k_bus_destroy(b);
}

int main(void) {
    /* T1: 動作中のタイマが無ければ INT_MAX */
    {
        x68k_bus_t *b = x68k_bus_create();
        assert(b != NULL);
        assert(x68k_bus_mfp_cycles_to_next_event(b) == INT_MAX);
        x68k_bus_destroy(b);
    }

    /* T2: 10MHz・TCDR=10・÷4 → 10 カウント × 4 × 2.5 = 100 サイクル */
    {
        x68k_bus_t *b = x68k_bus_create();
        mfp_reg_write8(b, REG_TCDR, 10u);
        mfp_reg_write8(b, REG_TCDCR, TCDCR_TC_DIV4);
        assert(x68k_bus_mfp_cycles_to_next_event(b) == 100);
        x68k_bus_destroy(b);
    }

    /* T3: 満了予定の厳密性（端数 frac が残る cpu_hz・prescale・TCDR=0(=256) を含む） */
    {
        static const uint32_t hz[] = {10000000u, 16000000u, 25000000u, 33000000u};
        static const uint8_t tcdr[] = {1u, 2u, 13u, 200u, 0u};
        static const uint8_t mode[] = {0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u};
        for (size_t i = 0; i < sizeof(hz) / sizeof(hz[0]); i++)
            for (size_t j = 0; j < sizeof(tcdr) / sizeof(tcdr[0]); j++)
                for (size_t k = 0; k < sizeof(mode) / sizeof(mode[0]); k++)
                    check_expiry_exact(hz[i], tcdr[j], mode[k]);
    }

    /* T4: 端数 frac 途中からの予定（途中まで進めてから再計算しても厳密） */
    {
        x68k_bus_t *b = x68k_bus_create();
        x68k_bus_set_cpu_hz(b, 25000000u);
        mfp_reg_write8(b, REG_IERB, 0x20u);
        mfp_reg_write8(b, REG_IMRB, 0x20u);
        mfp_reg_write8(b, REG_TCDR, 50u);
        mfp_reg_write8(b, REG_TCDCR, 0x70u); /* ÷200 */
        x68k_bus_mfp_tick(b, 777);
        int to_event = x68k_bus_mfp_cycles_to_next_event(b);
        x68k_bus_mfp_tick(b, to_event - 1);
        assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C));
        x68k_bus_mfp_tick(b, 1);
        assert(mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C));
        x68k_bus_destroy(b);
    }

    /* T5: Timer B / D も予定に含まれる（最小のものが返る） */
    {
        x68k_bus_t *b = x68k_bus_create();
        mfp_reg_write8(b, REG_TCDR, 200u);
        mfp_reg_write8(b, REG_TCDCR, 0x70u);   /* Timer C: 非常に長い */
        mfp_reg_write8(b, 0xE88021u, 0x0Du);   /* TBDR=13 */
        mfp_reg_write8(b, 0xE8801Bu, 0x01u);   /* Timer B ÷4: 130 サイクル */
        assert(x68k_bus_mfp_cycles_to_next_event(b) == 130);
        x68k_bus_destroy(b);
    }

    /* T6: レジスタ書込み前のサイクルを新設定へ遡及しない。
     * 10MHz・÷4 は 10 サイクル/カウント。書込み時点で 60 サイクル経過済み、
     * チャンク全体は 100 サイクル → 開始後に数えるのは 40 サイクル = 4 カウント。 */
    {
        x68k_bus_t *b = x68k_bus_create();
        mfp_reg_write8(b, REG_TCDR, 100u);
        x68k_bus_mfp_set_cycle_source(b, fake_cycle_source, NULL);
        x68k_bus_mfp_chunk_begin(b);
        s_fake_cycles = 60;
        mfp_reg_write8(b, REG_TCDCR, TCDCR_TC_DIV4);
        assert(b->mfp.tc == 100u);
        x68k_bus_mfp_chunk_tick(b, 100);
        assert(b->mfp.tc == 96u);

        /* T7: 次のチャンクでは同期済みサイクルが 0 に戻る。読み出し時点で
         * 70 サイクル経過 → その分が先に反映され、末尾では残り 30 のみ。 */
        x68k_bus_mfp_chunk_begin(b);
        s_fake_cycles = 70;
        (void)mfp_reg_read8(b, REG_TCDR);
        assert(b->mfp.tc == 89u); /* 96 - 7 */
        x68k_bus_mfp_chunk_tick(b, 100);
        assert(b->mfp.tc == 86u); /* 89 - 3 */

        /* T8: cycle_source 解除後は従来どおりチャンク末尾の一括 tick */
        x68k_bus_mfp_set_cycle_source(b, NULL, NULL);
        x68k_bus_mfp_chunk_begin(b);
        mfp_reg_write8(b, REG_TCDR, 100u);
        x68k_bus_mfp_chunk_tick(b, 100);
        assert(b->mfp.tc == 76u); /* 86 - 10 */
        x68k_bus_destroy(b);
    }

    puts("mfp_timer_sync_test: OK");
    return 0;
}
