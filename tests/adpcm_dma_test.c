/*
 * DMAC ch3(ADPCM)の実時間ペーシングをCPUチャンク単位に変更したことの単体
 * テスト。main.cのCPUチャンクループが呼ぶのと同じ粒度
 * （MFP_TICK_CYCLES=200）でx68k_bus_adpcm_tick()を繰り返し呼び、
 *   1. 平均転送レートが旧来のフレーム単位実装と一致すること
 *   2. 再武装から最初の1バイト転送までの遅延が数チャンク(≈128µs)に収まり、
 *      1フレーム(16.7ms)まるごと供給が止まる旧バグが再発しないこと
 *   3. 「budget==0なら1へ切り上げ」ハック相当の過剰供給が起きていないこと
 * を検証する。x68k_bus_createはROM不要（mfp_fmirq_test.cと同じ流儀）。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

#define TICK_CHUNK_CYCLES 200 /* main.cのMFP_TICK_CYCLESと同値 */

/* fs=15625Hz(8MHz/512)でDMAC ch3をchain==0(単純転送)、MTC=mtc、
 * MAR=0x00010000、DAR=0x00E92003(ADPCM固定)、CCR=ccrで武装する。 */
static void arm_simple_ccr(x68k_bus_t *b, uint16_t mtc, uint8_t ccr) {
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

    x68k_bus_write8(b, 0xE840C7u, ccr); /* CCR START(+必要ならCNT) */

    assert(b->dmac.ch3.active == 1);
    assert(b->dmac.ch3.remaining == mtc);
    assert(adpcm_get_sample_rate_hz(&b->adpcm) == 15625u);
}

static void arm_simple(x68k_bus_t *b, uint16_t mtc) {
    arm_simple_ccr(b, mtc, 0x88u); /* CCR START、CNTなし */
}

/* コンティニュー動作の次ブロック(BAR/BTC)を設定する。 */
static void set_base_block(x68k_bus_t *b, uint32_t bar, uint16_t btc) {
    x68k_bus_write8(b, 0xE840DAu, (uint8_t)(btc >> 8)); /* BTC hi */
    x68k_bus_write8(b, 0xE840DBu, (uint8_t)btc);        /* BTC lo */
    x68k_bus_write8(b, 0xE840DCu, (uint8_t)(bar >> 24));
    x68k_bus_write8(b, 0xE840DDu, (uint8_t)(bar >> 16));
    x68k_bus_write8(b, 0xE840DEu, (uint8_t)(bar >> 8));
    x68k_bus_write8(b, 0xE840DFu, (uint8_t)bar);
}

/* 1. 平均転送レートが旧フレーム単位実装(fs/120 = 130 bytes/frame @15625Hz)と
 *    一致すること。1フレーム分のチャンク(166600サイクル≈833チャンク)を
 *    回し、消費バイト数が130±1であることを確認する。 */
static void test_average_rate_matches_legacy(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    arm_simple(b, 2000);

    uint16_t before = (uint16_t)b->dmac.ch3.remaining;
    for (int i = 0; i < 833; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);
    uint16_t consumed = (uint16_t)(before - b->dmac.ch3.remaining);

    assert(consumed >= 129 && consumed <= 131);
    printf("test_average_rate_matches_legacy: consumed=%u (expected 130+-1)\n", consumed);
}

/* 2. 再武装直後、1バイト目が転送されるまでのチャンク数が7回以内であること
 *    (1280サイクル/バイト ÷ 200サイクル/チャンク = 6.4 → 7チャンクで到達)。
 *    かつ、それより前のチャンクでは一切転送されない(過剰供給ハックが
 *    残っていないことの回帰確認)。 */
static void test_first_byte_latency_and_no_oversupply(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    arm_simple(b, 1000);

    int first_transfer_chunk = -1;
    for (int i = 1; i <= 10; i++) {
        uint32_t before = b->dmac.ch3.remaining;
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);
        if (b->dmac.ch3.remaining != before) {
            first_transfer_chunk = i;
            break;
        }
    }
    assert(first_transfer_chunk >= 1 && first_transfer_chunk <= 7);
    /* 最初の6チャンク(1200サイクル<1280サイクル)では絶対に転送が起きない
     * ことも明示的に確認(「budget==0なら1」ハックが復活していないか)。 */
    assert(first_transfer_chunk >= 6);
    printf("test_first_byte_latency_and_no_oversupply: first_transfer_chunk=%d "
           "(expected 6 or 7)\n",
           first_transfer_chunk);
}

/* 3. 一時停止後の再武装の再現ケース: 再生中にCCR SAB(一時停止)→約4456サイクル
 *    (=フレーム境界を跨ぐのに十分な長さ)相当のtick空振り→CCR START(再武装)
 *    という流れの後、再武装から少数チャンクのうちに転送が再開すること。
 *    旧実装(フレーム先頭で1回だけtick)ではこの窓がフレーム境界を跨ぐと
 *    そのフレームが丸ごと0バイト転送になっていた(taken=0)。 */
static void test_resume_after_sab_rearm_within_frame_boundary(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    arm_simple(b, 1000);

    /* 転送を軌道に乗せる(50チャンク=10000サイクル≒7-8バイト分)。 */
    for (int i = 0; i < 50; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);
    uint32_t remaining_before_sab = b->dmac.ch3.remaining;
    assert(remaining_before_sab < 1000u); /* 転送が進んでいること */

    /* CCR SAB: 一時停止(active=0)。 */
    x68k_bus_write8(b, 0xE840C7u, 0x10u);
    assert(b->dmac.ch3.active == 0);

    /* 一時停止中のtick空振り(実機の4456サイクル相当、余裕を見て23チャンク=
     * 4600サイクル)。active==0なので何も進まないはず。 */
    for (int i = 0; i < 23; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);
    assert(b->dmac.ch3.remaining == remaining_before_sab); /* 停止中は不変 */

    /* CCR START: 再武装(MTCレジスタは直近のremainingを反映済みなので、
     * そこから再開する)。 */
    x68k_bus_write8(b, 0xE840C7u, 0x88u);
    assert(b->dmac.ch3.active == 1);
    assert(b->dmac.ch3.remaining == remaining_before_sab);

    /* 再武装から10チャンク(2000サイクル、1バイト分の1280サイクルを十分
     * 超える)以内に転送が再開しているはず(旧実装ならここが1フレーム分
     * 丸ごと0のまま=taken=0になっていた)。 */
    for (int i = 0; i < 10; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);
    assert(b->dmac.ch3.remaining < remaining_before_sab);
    printf("test_resume_after_sab_rearm_within_frame_boundary: remaining %u -> %u "
           "within 10 chunks after re-arm\n",
           remaining_before_sab, b->dmac.ch3.remaining);
}

/* 4. cpu_cycles<=0やinactive時にno-opであること(ガード節の回帰確認)。 */
static void test_inactive_and_nonpositive_cycles_are_noop(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* 未武装(active=0)状態でtickしても何も起きない。 */
    x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);
    assert(b->dmac.ch3.active == 0);

    arm_simple(b, 1000);
    uint32_t before = b->dmac.ch3.remaining;
    x68k_bus_adpcm_tick(b, 0);
    x68k_bus_adpcm_tick(b, -1);
    assert(b->dmac.ch3.remaining == before);
    printf("test_inactive_and_nonpositive_cycles_are_noop: OK\n");
}

/* 48バイトブロック(=307.2チャンク)を1つ転送しきるのに十分なチャンク数。
 * 余りの2バイト分は次ブロックの先頭へ入る。 */
#define CHUNKS_PER_48BYTE_BLOCK 320

/* 5. CCR.CNT(bit6)コンティニュー動作。PCM8A.Xと同じ48バイトのダブルバッファ
 *    手順で、ブロック完了時にBAR→MAR / BTC→MTCが取り込まれて転送が継続する
 *    こと、CSRはCOC(bit7)ではなくBTC(bit6=Block Transfer Complete)が立つこと、
 *    CNTが自動クリアされソフトウェアの再セットで次ブロックへ移れることを
 *    確認する。CNT未実装時はブロックごとにCOCが立ち、PCM8A.Xが演奏終了と
 *    判断してADPCMを停止していた。 */
static void test_cnt_continue_double_buffer(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    arm_simple_ccr(b, 48, 0xC8u); /* PCM8A.Xと同じ CCR=START+CNT+ITE */
    assert(b->dmac.ch3.cnt == 1);
    assert((x68k_bus_read8(b, 0xE840C7u) & 0x40u) != 0); /* CCR.CNTを読み返せる */
    assert((x68k_bus_read8(b, 0xE840C0u) & 0x08u) != 0); /* CSR.ACT: 動作中 */
    set_base_block(b, 0x00020000u, 48);

    for (int i = 0; i < CHUNKS_PER_48BYTE_BLOCK; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);

    uint8_t csr = x68k_bus_read8(b, 0xE840C0u);
    assert((csr & 0x40u) != 0); /* CSR.BTC: ブロック転送完了 */
    assert((csr & 0x80u) == 0); /* CSR.COC: チャンネル動作は終了していない */
    assert(b->dmac.ch3.active == 1);
    assert(b->dmac.ch3.cnt == 0); /* 継続のたびにハードウェアが自動クリア */
    assert(b->dmac.ch3_irq_pending == 1);
    /* MARがBARの示す第2ブロックへ移り、余り2バイト分だけ進んでいること。 */
    assert(b->dmac.ch3.mar >= 0x00020000u && b->dmac.ch3.mar < 0x00020030u);

    /* PCM8A.XのISR相当: CSRを#$FFでwrite-1-to-clearしてもACTは落ちない
     * （落ちるとドライバが「停止した」と誤判定してフルリスタートへ落ちる）。
     * その上で次ブロックを設定しCNTを再セットし、読み返して確認する。 */
    x68k_bus_write8(b, 0xE840C0u, 0xFFu);
    uint8_t csr_after_clear = x68k_bus_read8(b, 0xE840C0u);
    assert((csr_after_clear & 0x40u) == 0); /* BTCはクリアされる */
    assert((csr_after_clear & 0x08u) != 0); /* ACTは残る */
    set_base_block(b, 0x00030000u, 48);
    x68k_bus_write8(b, 0xE840C7u, 0x48u); /* CCR: CNT + ITE */
    assert(b->dmac.ch3.cnt == 1);
    assert((x68k_bus_read8(b, 0xE840C7u) & 0x40u) != 0); /* 読み返しで武装を確認 */
    assert(b->dmac.ch3.active == 1);

    for (int i = 0; i < CHUNKS_PER_48BYTE_BLOCK; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);

    assert(b->dmac.ch3.active == 1);
    assert(b->dmac.ch3.mar >= 0x00030000u && b->dmac.ch3.mar < 0x00030030u);
    printf("test_cnt_continue_double_buffer: MAR=0x%06X after 2 continues\n", b->dmac.ch3.mar);
}

/* 6. CNT非武装・武装できない条件の確認。
 *    - CNTなしでブロックを使い切ったら従来どおりCOCで終了する(既存動作の回帰)
 *    - 動作中でもSTART同時でもないCNTセットは無視する(タイミングエラー)
 *    - チェーンモードでのCNTセットは無視する(コンフィギュレーションエラー) */
static void test_cnt_guards(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    arm_simple(b, 48); /* CCR=START のみ、CNTなし */
    assert(b->dmac.ch3.cnt == 0);
    set_base_block(b, 0x00020000u, 48);
    for (int i = 0; i < CHUNKS_PER_48BYTE_BLOCK; i++)
        x68k_bus_adpcm_tick(b, TICK_CHUNK_CYCLES);

    uint8_t csr = x68k_bus_read8(b, 0xE840C0u);
    assert((csr & 0x80u) != 0); /* CSR.COC: 転送終了 */
    assert((csr & 0x40u) == 0); /* CSR.BTCは立たない */
    assert((csr & 0x08u) == 0); /* CSR.ACT: 終了したので落ちている */
    assert(b->dmac.ch3.active == 0);
    assert(b->dmac.ch3.mar < 0x00020000u); /* BARへは移っていない */

    /* 未武装状態でCNTだけ書いても武装されず、読み返しも0になる。 */
    x68k_bus_t *b2 = x68k_bus_create();
    assert(b2 != NULL);
    x68k_bus_write8(b2, 0xE840C7u, 0x48u);
    assert(b2->dmac.ch3.cnt == 0);
    assert((x68k_bus_read8(b2, 0xE840C7u) & 0x40u) == 0);

    /* チェーンモード(OCR.CHAIN=2)で動作中のCNTセットは無視される。 */
    x68k_bus_t *b3 = x68k_bus_create();
    assert(b3 != NULL);
    arm_simple(b3, 48);
    x68k_bus_write8(b3, 0xE840C5u, 0x08u); /* OCR: chain=2(アレイチェーン) */
    set_base_block(b3, 0x00020000u, 1);
    x68k_bus_write8(b3, 0xE840C7u, 0x88u); /* 再START: chain=2でラッチし直す */
    assert(b3->dmac.ch3.chain == 2);
    x68k_bus_write8(b3, 0xE840C7u, 0x48u); /* CNT: チェーンモードでは無視 */
    assert(b3->dmac.ch3.cnt == 0);
    assert((x68k_bus_read8(b3, 0xE840C7u) & 0x40u) == 0);
    printf("test_cnt_guards: OK\n");
}

int main(void) {
    test_average_rate_matches_legacy();
    test_first_byte_latency_and_no_oversupply();
    test_resume_after_sab_rearm_within_frame_boundary();
    test_inactive_and_nonpositive_cycles_are_noop();
    test_cnt_continue_double_buffer();
    test_cnt_guards();
    printf("adpcm_dma_test: all tests passed\n");
    return 0;
}
