/*
 * MFP Timer B(ch8)の単体テスト（docs/ix68k-internals/design/mfp_design.md §9）。
 * mfp_fmirq_test.c と同じソース構成・呼び出し流儀（x68k_bus_get_hooks() 経由の
 * iack フック）を踏襲する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);

    /* T1: TBCR/TBDRのwrite→read往復。TBCRは&0x0Fでマスクされて読める(RST/TBOが0に見える) */
    mfp_reg_write8(b, 0xE8801Bu, 0xF9u); /* RST/TBO込みで書いてもbit3-0だけ保持 */
    assert(mfp_reg_read8(b, 0xE8801Bu) == 0x09u);
    mfp_reg_write8(b, 0xE8801Bu, 0x00u); /* 停止に戻す */
    mfp_reg_write8(b, 0xE88021u, 0x0Du);
    assert(mfp_reg_read8(b, 0xE88021u) == 0x0Du);

    /* T2: 停止中(TBCR=$00)のTBDR書き込みでカウンタも即時更新される。
     * 動作中(TBCR=$01)の書き込みでは初期値のみ更新 */
    mfp_reg_write8(b, 0xE88021u, 0x20u);
    assert(b->mfp.tb == 0x20u);          /* 停止中は即時反映 */
    mfp_reg_write8(b, 0xE8801Bu, 0x01u); /* ディレイモード(÷4)で動作開始、TBDRからリスタート */
    assert(b->mfp.tb == 0x20u);
    mfp_reg_write8(b, 0xE88021u, 0x30u); /* 動作中はTBDRのみ更新、現在カウンタは変わらない */
    assert(b->mfp.tbdr == 0x30u);
    assert(b->mfp.tb == 0x20u);

    /* T3: TBCRに非ゼロモードを書くとカウンタがTBDRからリスタートする(fracも0に戻る) */
    b->mfp.tb = 0x05u;
    b->mfp.tb_frac = 123;
    mfp_reg_write8(b, 0xE8801Bu, 0x02u); /* ディレイモード(÷10)へ切替 */
    assert(b->mfp.tb == b->mfp.tbdr);
    assert(b->mfp.tb_frac == 0);

    /* T4: 周期の検算。TBCR=$01(÷4)・TBDR=$0D・cpu_hz=10MHz(既定)で
     * 4(prescale)×13(TBDR)×(10MHz/4MHz) = 130サイクルでpendingが立つ */
    mfp_reg_write8(b, 0xE8801Bu, 0x00u); /* 停止 */
    mfp_reg_write8(b, 0xE88021u, 0x0Du); /* TBDR=13、停止中なのでtbも即時13になる */
    mfp_reg_write8(b, 0xE88007u, 0x01u); /* IERA bit0 on */
    mfp_reg_write8(b, 0xE88013u, 0x01u); /* IMRA bit0 on */
    mfp_reg_write8(b, 0xE8801Bu, 0x01u); /* ÷4で動作開始、tb=13からリスタート */
    x68k_bus_mfp_tick(b, 129);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_B));
    x68k_bus_mfp_tick(b, 1);
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_B));

    /* T5: IERA bit0=0のときIPRA bit0もpendingも立たない（実機のシステム設定値がこれ） */
    mfp_ch_pending_clear(&b->mfp, MFP_CH_TIMER_B);
    b->mfp.ipra = 0;
    mfp_reg_write8(b, 0xE88007u, 0x00u); /* IERA bit0 off */
    mfp_reg_write8(b, 0xE8801Bu, 0x00u);
    mfp_reg_write8(b, 0xE88021u, 0x01u);
    mfp_reg_write8(b, 0xE8801Bu, 0x01u);
    x68k_bus_mfp_tick(b, 1000);
    assert((b->mfp.ipra & 0x01u) == 0u);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_B));

    /* T6: IERA bit0=1 / IMRA bit0=0のときIPRA bit0は立つがpendingは立たない */
    b->mfp.ipra = 0;
    mfp_reg_write8(b, 0xE88007u, 0x01u); /* IERA bit0 on */
    mfp_reg_write8(b, 0xE88013u, 0x00u); /* IMRA bit0 off */
    mfp_reg_write8(b, 0xE8801Bu, 0x00u);
    mfp_reg_write8(b, 0xE88021u, 0x01u);
    mfp_reg_write8(b, 0xE8801Bu, 0x01u);
    x68k_bus_mfp_tick(b, 1000);
    assert((b->mfp.ipra & 0x01u) != 0u);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_B));

    /* T7: IACK: Timer B単独pending → VR|8を返しIPRA bit0とpendingがクリアされる */
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_B);
    b->mfp.ipra |= 0x01u;
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 8u));
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_B));
    assert((b->mfp.ipra & 0x01u) == 0u);

    /* T8: IACK優先度: USART RX(12) > Timer B(8) > GPIP4(6) > Timer C(5) > FMIRQ(3) */
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_B);
    mfp_ch_pending_set(&b->mfp, MFP_CH_GPIP4);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    mfp_ch_pending_set(&b->mfp, MFP_CH_FMIRQ);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 12u));
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 8u));
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 6u));
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 5u));
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 3u));
    assert(hooks.iack(b, 6) == 0);

    /* T9: IMRA書き込みでの個別再評価。Timer A(bit5)とTimer B(bit0)を同時pendingにし、
     * bit5だけマスクしてもTimer Bのpendingが残ること
     * （実装中に発見した取り違えバグの同型回帰）。 */
    b->mfp.imra = 0x21u; /* bit5(TimerA) + bit0(TimerB) */
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_A);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_B);
    b->mfp.ipra |= 0x21u;
    mfp_reg_write8(b, 0xE88013u, 0x01u); /* bit5をマスク、bit0は維持 */
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_A));
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_B));
    mfp_ch_pending_clear(&b->mfp, MFP_CH_TIMER_B);

    /* T10: イベントカウント(TBCR=$08)・パルス幅測定(TBCR=$09)でカウンタが動かない
     * （X68000ではTBIがLレベル固定のため停止扱い） */
    b->mfp.tb = 5;
    mfp_reg_write8(b, 0xE8801Bu, 0x08u);
    x68k_bus_mfp_tick(b, 100000);
    assert(b->mfp.tb == 5);
    mfp_reg_write8(b, 0xE8801Bu, 0x09u);
    x68k_bus_mfp_tick(b, 100000);
    assert(b->mfp.tb == 5);

    /* T11: TBDR=0のとき256カウント周期になること
     * （8bitカウンタの自然なラップ。256×4(prescale)×(10/4) = 2560サイクル） */
    mfp_reg_write8(b, 0xE8801Bu, 0x00u); /* 停止 */
    mfp_reg_write8(b, 0xE88021u, 0x00u); /* TBDR=0 */
    mfp_reg_write8(b, 0xE8801Bu, 0x01u); /* ÷4で動作開始、tb=0からリスタート */
    x68k_bus_mfp_tick(b, 2559);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_B));
    x68k_bus_mfp_tick(b, 1);
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_B));

    x68k_bus_destroy(b);
    printf("mfp_timerb_test: OK\n");
    return 0;
}
