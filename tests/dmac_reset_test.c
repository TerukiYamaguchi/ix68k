/*
 * DMAC(HD63450)がRESET命令(x68k_bus_reset_peripheral())で正しく初期化される
 * ことの単体テスト。回路図でHD63450のRESETピンがCPU RESET信号に
 * 直結していることを確認済みだが、従来はch3(ADPCM)のCSR/CCR/active/paused/cnt
 * しか落としておらず、ch0-ch2の全レジスタ・ch0(FDC)武装フラグ・ch3の転送途中
 * 状態がソフトリセットをまたいで残留していた。dmac_reset()実装の回帰確認用。
 * x68k_bus_createはROM不要（mfp_fmirq_test.c/adpcm_dma_test.cと同じ流儀）。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

#define TICK_CHUNK_CYCLES 200 /* main.cのMFP_TICK_CYCLESと同値 */

static const uint32_t kChBase[4] = {0x4000u, 0x4040u, 0x4080u, 0x40C0u};

/* chベースオフセット$40バイト全域へ番兵パターンを敷き詰める。CSR/CCRも含め
 * 生バイトとして直接crtc_regsへ書く(dmac_write8の副作用を経由せず、単に
 * 「リセットで非ゼロが残っていないこと」だけを確認したいため)。 */
static void poison_channel(x68k_bus_t *b, uint32_t ch_base, uint8_t pattern) {
    for (uint32_t off = 0; off < 0x40u; off++)
        b->crtc_regs[ch_base + off] = pattern;
}

/* 1. 全ch・全レジスタがリセットでゼロクリアされ、NIV(+$25)/EIV(+$27)だけ
 *    実機のRESET後の値$0F(未初期化割り込みベクタ番号15相当)になること。 */
static void test_all_channels_registers_cleared(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    for (int i = 0; i < 4; i++)
        poison_channel(b, kChBase[i], (uint8_t)(0x11u * (uint32_t)(i + 1)));

    x68k_bus_reset_peripheral(b);

    for (int i = 0; i < 4; i++) {
        for (uint32_t off = 0; off < 0x40u; off++) {
            uint8_t v = b->crtc_regs[kChBase[i] + off];
            if (off == 0x25u || off == 0x27u)
                assert(v == 0x0Fu);
            else
                assert(v == 0u);
        }
    }
    printf("test_all_channels_registers_cleared: OK\n");
}

/* 2. ch0(FDC)のCCR START武装フラグ(ch0_armed)がリセットで解除されること。
 *    従来はSAB(CCR bit4)経路以外で落とす手段が無く、RESETをまたいで残留
 *    していた。 */
static void test_ch0_armed_cleared(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE84007u, 0x80u); /* ch0 CCR: bit7=START */
    assert(b->dmac.ch0_armed == 1);

    x68k_bus_reset_peripheral(b);
    assert(b->dmac.ch0_armed == 0);
    printf("test_ch0_armed_cleared: OK\n");
}

/* fs=15625Hz(8MHz/512)でDMAC ch3をchain==0(単純転送)、MTC=mtc、
 * MAR=0x00010000、DAR=0x00E92003(ADPCM固定)で武装する（adpcm_dma_test.cの
 * arm_simple_ccrと同じ手順）。 */
static void arm_ch3_simple(x68k_bus_t *b, uint16_t mtc) {
    adpcm_ppi_write8(&b->adpcm, 0x05u, 0x08u); /* PPI PortC: rate_bits=10b→divider512 */
    adpcm_opm_write8(&b->adpcm, 0x01u, 0x1Bu); /* OPM $1B選択 */
    adpcm_opm_write8(&b->adpcm, 0x03u, 0x00u); /* CT1=0→8MHz base、fs=8000000/512=15625 */

    x68k_bus_write8(b, 0xE840C5u, 0x00u);               /* OCR: dir=0(mem→dev), chain=0 */
    x68k_bus_write8(b, 0xE840C6u, 0x04u);               /* SCR: mac=1(++), dac=0(固定) */
    x68k_bus_write8(b, 0xE840CAu, (uint8_t)(mtc >> 8)); /* MTC hi */
    x68k_bus_write8(b, 0xE840CBu, (uint8_t)mtc);        /* MTC lo */
    x68k_bus_write8(b, 0xE840CDu, 0x01u);               /* MAR bits23-16 */
    x68k_bus_write8(b, 0xE840CEu, 0x00u);               /* MAR bits15-8 */
    x68k_bus_write8(b, 0xE840CFu, 0x00u);               /* MAR bits7-0 → 0x00010000 */
    x68k_bus_write8(b, 0xE840D4u, 0x00u); /* DAR 4バイト → 0x00E92003(ADPCM固定) */
    x68k_bus_write8(b, 0xE840D5u, 0xE9u);
    x68k_bus_write8(b, 0xE840D6u, 0x20u);
    x68k_bus_write8(b, 0xE840D7u, 0x03u);

    x68k_bus_write8(b, 0xE840C7u, 0x88u); /* CCR START */
    assert(b->dmac.ch3.active == 1);
}

/* 3. ch3(ADPCM)が転送途中(active)のままRESETされたとき、転送状態が完全に
 *    初期化され、リセット後はtickしてもゲストRAMが変化しないこと。 */
static void test_ch3_in_progress_transfer_aborted(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    arm_ch3_simple(b, 2000);

    for (int i = 0; i < 50; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);
    assert(b->dmac.ch3.remaining < 2000u); /* 転送が進んでいること */

    x68k_bus_reset_peripheral(b);

    assert(b->dmac.ch3.active == 0);
    assert(b->dmac.ch3.paused == 0);
    assert(b->dmac.ch3.cnt == 0);
    assert(b->dmac.ch3.remaining == 0);
    assert(b->dmac.ch3.mar == 0);
    assert(b->dmac.ch3.budget_acc == 0);
    assert((x68k_bus_read8(b, 0xE840C0u) & 0x08u) == 0); /* CSR.ACT: 落ちている */

    uint8_t before[16];
    for (int i = 0; i < 16; i++)
        before[i] = x68k_bus_read8(b, 0x00010000u + (uint32_t)i);
    for (int i = 0; i < 100; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);
    for (int i = 0; i < 16; i++)
        assert(x68k_bus_read8(b, 0x00010000u + (uint32_t)i) == before[i]);
    printf("test_ch3_in_progress_transfer_aborted: OK\n");
}

/* 4. ch2(汎用メモリ間転送)の完了割り込み(CSR.COC + ch2_irq_pending)がRESETで
 *    クリアされること。転送途中のリセットは dmac_ch2_timing_test.c で確認する。 */
static void test_ch2_completion_irq_cleared(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE84084u, 0x08u); /* DCR: 16-bit port */
    x68k_bus_write8(b, 0xE84085u, 0x00u); /* OCR: dir=0(mem→dev), chain=0 */
    x68k_bus_write8(b, 0xE84086u, 0x00u); /* SCR: mac=0(固定), dac=0(固定) */
    x68k_bus_write8(b, 0xE8408Au, 0x00u); /* MTC hi */
    x68k_bus_write8(b, 0xE8408Bu, 0x01u); /* MTC lo = 1バイト */
    x68k_bus_write8(b, 0xE8408Cu, 0x00u); /* MAR = 0x00000000 */
    x68k_bus_write8(b, 0xE8408Du, 0x00u);
    x68k_bus_write8(b, 0xE8408Eu, 0x00u);
    x68k_bus_write8(b, 0xE8408Fu, 0x00u);
    x68k_bus_write8(b, 0xE84094u, 0x00u); /* DAR = 0x00000001 */
    x68k_bus_write8(b, 0xE84095u, 0x00u);
    x68k_bus_write8(b, 0xE84096u, 0x00u);
    x68k_bus_write8(b, 0xE84097u, 0x01u);

    x68k_bus_write8(b, 0xE84087u, 0x88u); /* CCR START */
    assert(b->dmac.ch2_irq_pending == 0); /* 転送時間が経過するまで完了しない */
    x68k_bus_dmac_tick(b, x68k_bus_dmac_cycles_to_next_event(b));
    assert(b->dmac.ch2_irq_pending == 1);
    assert((x68k_bus_read8(b, 0xE84080u) & 0x80u) != 0); /* CSR.COC */

    x68k_bus_reset_peripheral(b);
    assert(b->dmac.ch2_irq_pending == 0);
    assert((x68k_bus_read8(b, 0xE84080u) & 0x80u) == 0);
    printf("test_ch2_completion_irq_cleared: OK\n");
}

/* 5. RESETがDMAC以外のcrtc_regs領域(CRTC本体・パレットRAM・アクションポート)
 *    を巻き込まないこと（dmac_reset()の$4000-$40FF一括クリアが範囲外へ
 *    はみ出していないことの回帰ガード）。 */
static void test_reset_does_not_touch_unrelated_regions(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    b->crtc_regs[0x00u] = 0xABu;   /* CRTC R00 */
    b->crtc_regs[0x2Fu] = 0xCDu;   /* DMAC直前の境界値 */
    b->crtc_regs[0x2000u] = 0xEFu; /* パレットRAM */
    b->crtc_regs[0x481u] = 0x08u;  /* アクションポート */

    x68k_bus_reset_peripheral(b);

    assert(b->crtc_regs[0x00u] == 0xABu);
    assert(b->crtc_regs[0x2Fu] == 0xCDu);
    assert(b->crtc_regs[0x2000u] == 0xEFu);
    assert(b->crtc_regs[0x481u] == 0x08u);
    printf("test_reset_does_not_touch_unrelated_regions: OK\n");
}

int main(void) {
    test_all_channels_registers_cleared();
    test_ch0_armed_cleared();
    test_ch3_in_progress_transfer_aborted();
    test_ch2_completion_irq_cleared();
    test_reset_does_not_touch_unrelated_regions();
    printf("dmac_reset_test: all tests passed\n");
    return 0;
}
