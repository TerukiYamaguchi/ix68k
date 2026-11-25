/*
 * MFP GPIP3(FMIRQ)割り込み配線の単体テスト
 * （docs/ix68k-internals/design/opm_design.md §4）。bus_iack()自体はx68k_bus.c内で
 * staticのため、x68k_bus_get_hooks()経由のmc68k_bus_t.iackフックとして
 * 間接的に呼ぶ。IRQ6内優先順位 USART(12) > Timer C(5) > FMIRQ(3) を検証する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);

    b->mfp.vr = 0x40u; /* 実機はROMが設定するが、テストでは直接置く */

    /* A. 何もpendingでなければ0(オートベクタ) */
    assert(hooks.iack(b, 6) == 0);

    /* B. USART RX単独 -> VR|12、処理後クリア */
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 12u));
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_USART_RX));

    /* C. Timer C単独 -> VR|5、処理後クリア */
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 5u));
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C));

    /* D. FMIRQ単独 -> VR|3、処理後クリア(IPRB bit3も落ちる) */
    mfp_ch_pending_set(&b->mfp, MFP_CH_FMIRQ);
    b->mfp.iprb |= 0x08u;
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 3u));
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_FMIRQ));
    assert((b->mfp.iprb & 0x08u) == 0);

    /* E. 3つ同時pending時の優先順位: USART(12) > Timer C(5) > FMIRQ(3) */
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    mfp_ch_pending_set(&b->mfp, MFP_CH_FMIRQ);

    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 12u));
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C));
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_FMIRQ));

    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 5u));
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_FMIRQ));

    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 3u));
    assert(hooks.iack(b, 6) == 0); /* 全部処理済み */

    /* F. x68k_bus_mfp_opmirq()経由の実配線: IERB/IMRB bit3が立っていないと
     * pendingへ昇格しない。 */
    x68k_bus_mfp_opmirq(b, 1);
    assert(hooks.iack(b, 6) == 0);

    b->mfp.ierb |= 0x08u;
    b->mfp.imrb |= 0x08u;
    x68k_bus_mfp_opmirq(b, 1);
    assert(hooks.iack(b, 6) == (uint8_t)(b->mfp.vr | 3u));

    /* asserted=0(FRESET等でOPM線が下がった)でIPRB/pendingが即座にクリアされる。 */
    x68k_bus_mfp_opmirq(b, 1);
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_FMIRQ));
    x68k_bus_mfp_opmirq(b, 0);
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_FMIRQ));
    assert((b->mfp.iprb & 0x08u) == 0);

    /* G. IMRB書き込みでソースごとに独立してpendingが再評価される
     * (Timer C/FMIRQ同時pending時、片方のIMRBビットだけ落としても
     * もう片方に影響しないことを確認。実装中に発見・修正した取り違え
     * バグの回帰確認)。 */
    mfp_ch_pending_set(&b->mfp, MFP_CH_TIMER_C);
    x68k_bus_mfp_opmirq(b, 1); /* FMIRQもpendingにする(IERB/IMRB bit3は上でON済み) */
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C) &&
           mfp_ch_pending_test(&b->mfp, MFP_CH_FMIRQ));
    b->mfp.imrb &= ~0x20u;                                 /* Timer C(bit5)のみマスク */
    mfp_reg_write8(b, 0xE88015u, b->mfp.imrb);             /* IMRB書き込みハンドラを経由 */
    assert(!mfp_ch_pending_test(&b->mfp, MFP_CH_TIMER_C)); /* Timer Cはクリアされる */
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_FMIRQ));    /* FMIRQは影響を受けない */

    /* H. x68k_bus_mfp_pending_irq()がFMIRQ単独でも真を返す。 */
    mfp_ch_pending_clear(&b->mfp, MFP_CH_FMIRQ);
    assert(x68k_bus_mfp_pending_irq(b) == 0);
    mfp_ch_pending_set(&b->mfp, MFP_CH_FMIRQ);
    assert(x68k_bus_mfp_pending_irq(b) != 0);

    /* I. GPIP bit3(FMIRQ)の実状態反映
     * （mfp_design.md §9）。
     * G1/G5/G6: 16位相全部でbit0/bit5/bit3/bit6(ラスター連動未実装=常時1)が期待どおりで、
     * GPIP読み出しがIPRA/IPRB/pendingを一切変えないこと（回帰確認）。 */
    b->mfp.ipra = 0;
    b->mfp.iprb = 0;
    for (int phase = 0; phase < 16; phase++) {
        uint8_t before_ipra = b->mfp.ipra, before_iprb = b->mfp.iprb;
        int pending_before = x68k_bus_mfp_pending_irq(b);
        uint8_t gpip = mfp_reg_read8(b, 0xE88001u);
        assert((gpip & 0x01u) != 0u); /* bit0=1: RTC ALARM未出力時のプルアップ */
        assert((gpip & 0x20u) != 0u); /* bit5=1固定(未使用) */
        assert((gpip & 0x08u) != 0u); /* bit3=1(fmirq_line=0のため) */
        assert((gpip & 0x40u) != 0u); /* bit6=1(ラスター連動未実装) */
        assert(b->mfp.ipra == before_ipra && b->mfp.iprb == before_iprb);
        assert(x68k_bus_mfp_pending_irq(b) == pending_before);
    }

    /* G2: b->opm.fmirq_lineにbit3が追従し、IERB/IMRB bit3の設定に依存しない
     * （GPIPはレベルでありpendingではない。IERB/IMRB bit3は上のテストで既にON済み）。 */
    b->opm.fmirq_line = 1;
    assert((mfp_reg_read8(b, 0xE88001u) & 0x08u) == 0u);
    b->opm.fmirq_line = 0;
    assert((mfp_reg_read8(b, 0xE88001u) & 0x08u) != 0u);

    /* G3/G4: GPIP bit6(CIRQ)の実状態反映
     * （mfp_design.md §9）。R09(crtc_regs[0x12/0x13])とraster.lineが
     * 一致する間だけbit6=0、raster.valid=0なら常時HIGH(1)にフォールバックする。 */
    b->crtc_regs[0x12] = 0x00u;
    b->crtc_regs[0x13] = 0x64u; /* R09 = 100 */
    b->crtc.raster.valid = 1;
    b->crtc.raster.line = 100;
    assert((mfp_reg_read8(b, 0xE88001u) & 0x40u) == 0u); /* G3: 一致でbit6=0 */
    b->crtc.raster.line = 101;
    assert((mfp_reg_read8(b, 0xE88001u) & 0x40u) != 0u); /* G3: 不一致でbit6=1 */
    b->crtc.raster.valid = 0;
    b->crtc.raster.line = 100;
    assert((mfp_reg_read8(b, 0xE88001u) & 0x40u) != 0u); /* G4: valid=0はフォールバック */

    /* G7/G8/G9: GPIP bit7(H-SYNC)/bit4(V-DISP)の役割是正
     * （mfp_design.md §5.1/§9）。raster.valid=0時の位相ハックのみを
     * 対象にする（ラスター由来化は別テストで扱う）。 */
    {
        int bit4_low = 0, bit4_high = 0, bit7_low = 0, bit7_high = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t gpip = mfp_reg_read8(b, 0xE88001u);
            if (gpip & 0x10u)
                bit4_high++;
            else
                bit4_low++;
            if (gpip & 0x80u)
                bit7_high++;
            else
                bit7_low++;
        }
        assert(bit4_low == 1);                 /* G7: bit4(V-DISP)は16回に1回だけLOW */
        assert(bit7_low == 8);                 /* G7: bit7(H-SYNC)は2回に1回LOW */
        assert(bit4_high > 0 && bit4_low > 0); /* G9: 両方観測される(ハング防止) */
        assert(bit7_high > 0 && bit7_low > 0); /* G9: 同上 */
    }
    /* G8: bit0/bit5/bit3/bit6が従来どおり（bit3/bit6はG2/G4でfmirq_line=0・valid=0の
     * ままなので、それぞれ常時1になる） */
    for (int i = 0; i < 16; i++) {
        uint8_t gpip = mfp_reg_read8(b, 0xE88001u);
        assert((gpip & 0x01u) != 0u);
        assert((gpip & 0x20u) != 0u);
        assert((gpip & 0x08u) != 0u);
        assert((gpip & 0x40u) != 0u);
    }

    x68k_bus_destroy(b);
    printf("mfp_fmirq_test: OK\n");
    return 0;
}
