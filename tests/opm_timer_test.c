/*
 * OPM内蔵タイマA/Bの単体テスト
 * （opm_design.md §4）。opm.c/ym2151.cのみリンク、
 * バス/CPU非依存。MFP(GPIP3/FMIRQ)への通知は別テスト(mfp_fmirq_test.c)の対象のため、
 * ここではym2151_status()/opm_status_read8()で観測できる範囲のみ検証する。
 */
#include "peripheral/opm.h"
#include "audio/ym2151.h"
#include <assert.h>
#include <stdio.h>

/* CLKA=1023(10bit最大)で最短周期64クロックに設定し、LOAD+IRQEN Aを立てる。
 * $10=CLKA上位8bit=0xFF, $11=CLKA下位2bit=0x03。 */
static void setup_timer_a_min_period(opm_t *o, int irqen) {
    opm_reg_write8(o, 0x01, 0x10);
    opm_reg_write8(o, 0x03, 0xFF);
    opm_reg_write8(o, 0x01, 0x11);
    opm_reg_write8(o, 0x03, 0x03);
    opm_reg_write8(o, 0x01, 0x14);
    opm_reg_write8(o, 0x03, (uint8_t)(0x01 | (irqen ? 0x04 : 0x00))); /* LOAD A + (IRQEN A) */
}

int main(void) {
    /* A. Timer Aオーバーフローでステータスフラグが立つ(64クロック=160CPUサイクル,
     * 1CPUサイクル=0.4OPMクロック)。 */
    {
        opm_t o;
        opm_reset(&o);
        setup_timer_a_min_period(&o, 1);
        assert((ym2151_status(&o.core) & 0x01u) == 0);
        opm_tick(&o, 160);
        assert((ym2151_status(&o.core) & 0x01u) != 0);
    }

    /* B. FRESET Aでフラグが下りる。 */
    {
        opm_t o;
        opm_reset(&o);
        setup_timer_a_min_period(&o, 1);
        opm_tick(&o, 160);
        assert((ym2151_status(&o.core) & 0x01u) != 0);
        opm_reg_write8(&o, 0x01, 0x14);
        opm_reg_write8(&o, 0x03, 0x11); /* LOAD A維持(bit0) + FRESET A(bit4) */
        assert((ym2151_status(&o.core) & 0x01u) == 0);
    }

    /* C. IRQEN OFFなら新規オーバーフローでもフラグが立たない
     * (IRQENはフラグセットのゲートであり、既に立ったフラグをマスクする
     * ものではないことに注意。ここではオーバーフロー前からIRQEN OFFにする)。 */
    {
        opm_t o;
        opm_reset(&o);
        setup_timer_a_min_period(&o, 0); /* IRQEN A off */
        opm_tick(&o, 160);
        assert((ym2151_status(&o.core) & 0x01u) == 0);
    }

    /* D. 一度立てたフラグはIRQENを後からOFFにしても残り続ける
     * (IRQENは将来のオーバーフローのゲートであり、既存フラグのマスクでは
     * ない、opm_design.md §3の実装判断)。 */
    {
        opm_t o;
        opm_reset(&o);
        setup_timer_a_min_period(&o, 1);
        opm_tick(&o, 160);
        assert((ym2151_status(&o.core) & 0x01u) != 0);
        opm_reg_write8(&o, 0x01, 0x14);
        opm_reg_write8(&o, 0x03, 0x01); /* LOAD A維持、IRQEN A OFF、FRESET無し */
        assert((ym2151_status(&o.core) & 0x01u) != 0); /* 既存フラグは残る */
    }

    /* E. 粒度不変性: 同じ総CPUサイクル数を粗い/細かいチャンクに分けても
     * 最終的なタイマステータスは一致する(テンポ破綻回帰の番人)。 */
    {
        opm_t o_coarse, o_fine;
        opm_reset(&o_coarse);
        opm_reset(&o_fine);
        setup_timer_a_min_period(&o_coarse, 1);
        setup_timer_a_min_period(&o_fine, 1);

        opm_tick(&o_coarse, 400); /* 1回で400サイクル(=2周期分オーバーフロー) */
        for (int i = 0; i < 40; i++)
            opm_tick(&o_fine, 10); /* 40回×10サイクル=400サイクル */

        assert(ym2151_status(&o_coarse.core) == ym2151_status(&o_fine.core));
    }

    /* F. BUSY: データ書き込み直後はBUSYが立ち、64クロック(=160サイクル)後に
     * 下りる。REG_MODE(0x14)書き込みはBUSYをセットしない。 */
    {
        opm_t o;
        opm_reset(&o);
        opm_reg_write8(&o, 0x01, 0x60); /* アドレスラッチ(A0=0書き込み)はBUSY対象外 */
        opm_reg_write8(&o, 0x03, 0x00); /* データ書き込み(TL=0) */
        assert((opm_status_read8(&o) & 0x80u) != 0);
        opm_tick(&o, 160);
        assert((opm_status_read8(&o) & 0x80u) == 0);

        opm_reg_write8(&o, 0x01, 0x14);
        opm_reg_write8(&o, 0x03, 0x00); /* REG_MODE書き込みはBUSYをセットしない */
        assert((opm_status_read8(&o) & 0x80u) == 0);
    }

    printf("opm_timer_test: OK\n");
    return 0;
}
