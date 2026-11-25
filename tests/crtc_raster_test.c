/*
 * CRTCラスターモデルの単体テスト
 * （docs/ix68k-internals/design/crtc_design.md §6.3）。
 * CPUを一切動かさず、x68k_bus_crtc_raster_advance()/flush()にサイクル数を流し込むだけで
 * 走査線位置追跡のドリフト・妥当性判定・MFP割り込み判定への配線を検証する。
 *
 * 縦スクロールのラッチ値（raster.tx_y_zero/gr_y_zero[]）はまだ
 * x68k_bus_get_tvram_scroll_y()等の公開getterからは参照されないため、
 * テストHはbus_internal.hのrasterフィールドを直接見て検証する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

static void set_crtc_reg(x68k_bus_t *b, int reg_num, uint16_t val) {
    b->crtc_regs[reg_num * 2] = (uint8_t)(val >> 8);
    b->crtc_regs[reg_num * 2 + 1] = (uint8_t)(val & 0xFFu);
}

/* 31.5kHz/512ライン標準設定。lines_per_frame = R04+1 なので、567走査線
 * （旧MFP_LINES_PER_FRAME定数と一致）にするには R04=566 を書く。
 * R05=16(vsync end), R06=40(vdisp_start-1), R07=552(vdisp end)。 */
static void setup_standard_crtc(x68k_bus_t *b) {
    set_crtc_reg(b, 4, 566);
    set_crtc_reg(b, 5, 16);
    set_crtc_reg(b, 6, 40);
    set_crtc_reg(b, 7, 552);
}

/* cycles_per_frame分をchunk刻みでadvance()し続け、返ってきたラスタ番号が
 * 0から連続昇順でlines_per_frame-1まで、合計lines_per_frame本になることを確認する。
 * 呼び出し元でframe_start済みであること。 */
static int drive_frame(x68k_bus_t *b, int cycles_per_frame, int chunk, int lines_per_frame) {
    int remaining = cycles_per_frame;
    int total = 0;
    int expect_next = 0;
    while (remaining > 0) {
        int c = remaining < chunk ? remaining : chunk;
        int lines[64];
        int n = x68k_bus_crtc_raster_advance(b, c, lines, 64);
        int cap = n < 64 ? n : 64;
        for (int i = 0; i < cap; i++) {
            assert(lines[i] == expect_next);
            expect_next++;
        }
        total += n;
        remaining -= c;
    }
    (void)lines_per_frame;
    return total;
}

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* A/B: ドリフトゼロ・走査線番号の連続性（10フレーム連続） */
    setup_standard_crtc(b);
    for (int frame = 0; frame < 10; frame++) {
        int cycles_per_frame = 10000000 / 60; /* 10MHz */
        x68k_bus_crtc_raster_frame_start(b, cycles_per_frame, 512);
        assert(x68k_bus_crtc_raster_valid(b) == 1);
        int total = drive_frame(b, cycles_per_frame, 200, 567);
        assert(total == 567);
        assert(b->crtc.raster.line == 566);
    }

    /* C: 表示行マッピング（dup=1） */
    x68k_bus_crtc_raster_frame_start(b, 10000000 / 60, 512);
    assert(x68k_bus_crtc_raster_valid(b) == 1);
    assert(x68k_bus_crtc_raster_display_row(b, 41) == 0);
    assert(x68k_bus_crtc_raster_display_row(b, 552) == 511);
    assert(x68k_bus_crtc_raster_display_row(b, 40) == -1);
    assert(x68k_bus_crtc_raster_display_row(b, 553) == -1);

    /* D: dup=2（vdisp_lines=512, display_h=256） */
    x68k_bus_crtc_raster_frame_start(b, 10000000 / 60, 256);
    assert(x68k_bus_crtc_raster_valid(b) == 1);
    assert(x68k_bus_crtc_raster_display_row(b, 41) == 0);
    assert(x68k_bus_crtc_raster_display_row(b, 42) == -1);
    assert(x68k_bus_crtc_raster_display_row(b, 43) == 1);

    /* E: 妥当性判定 */
    {
        x68k_bus_t *e = x68k_bus_create();
        assert(e != NULL);
        /* R04-R07 全0（起動直後相当） */
        x68k_bus_crtc_raster_frame_start(e, 10000000 / 60, 512);
        assert(x68k_bus_crtc_raster_valid(e) == 0);

        /* R06 >= R07 */
        set_crtc_reg(e, 4, 566);
        set_crtc_reg(e, 5, 16);
        set_crtc_reg(e, 6, 600);
        set_crtc_reg(e, 7, 552);
        x68k_bus_crtc_raster_frame_start(e, 10000000 / 60, 512);
        assert(x68k_bus_crtc_raster_valid(e) == 0);

        /* vdisp_linesがdisplay_hの非整数倍 */
        setup_standard_crtc(e); /* vdisp_lines=512 */
        x68k_bus_crtc_raster_frame_start(e, 10000000 / 60, 300);
        assert(x68k_bus_crtc_raster_valid(e) == 0);

        x68k_bus_destroy(e);
    }

    /* F: MFP ch14の発火（ドリフトで取りこぼされないこと） */
    setup_standard_crtc(b);
    set_crtc_reg(b, 9, 100); /* R09 = ラスタ一致比較値 */
    b->mfp.iera |= 0x40u;    /* IER-A bit6 = ラスタ一致イネーブル */
    b->mfp.imra |= 0x40u;    /* IMR-A bit6 */
    mfp_ch_pending_clear(&b->mfp, MFP_CH_RASTER);
    {
        int cycles_per_frame = 10000000 / 60;
        x68k_bus_crtc_raster_frame_start(b, cycles_per_frame, 512);
        int lines[64];
        x68k_bus_crtc_raster_flush(b, lines, 64);
    }
    assert(mfp_ch_pending_test(&b->mfp, MFP_CH_RASTER));

    /* G: CPUクロック非依存 */
    {
        const uint32_t hz_list[] = {10000000u, 16000000u, 25000000u, 33000000u, 50000000u};
        for (int i = 0; i < 5; i++) {
            setup_standard_crtc(b);
            int cycles_per_frame = (int)(hz_list[i] / 60u);
            x68k_bus_crtc_raster_frame_start(b, cycles_per_frame, 512);
            assert(x68k_bus_crtc_raster_valid(b) == 1);
            int total = drive_frame(b, cycles_per_frame, 200, 567);
            assert(total == 567);
        }
    }

    /* H: 縦スクロールのラッチ点（raster内部フィールドを直接検証） */
    setup_standard_crtc(b);
    set_crtc_reg(b, 11, 100); /* R11 = テキストYスクロール */
    {
        int cycles_per_frame = 10000000 / 60;
        x68k_bus_crtc_raster_frame_start(b, cycles_per_frame, 512);
        assert(b->crtc.raster.y_latched == 0);

        /* vdisp_start(=41)の直前まで進める: まだラッチされていない */
        int lines[64];
        int cycles_to_line40 = (int)((int64_t)40 * cycles_per_frame / 567) + 1;
        x68k_bus_crtc_raster_advance(b, cycles_to_line40, lines, 64);
        assert(b->crtc.raster.line < 41);
        assert(b->crtc.raster.y_latched == 0);

        /* vdisp_start到達後: ラッチされ、R11=100が反映される */
        while (b->crtc.raster.line < 41) {
            x68k_bus_crtc_raster_advance(b, 50, lines, 64);
        }
        assert(b->crtc.raster.y_latched == 1);
        assert(b->crtc.raster.tx_y_zero == 100);

        /* フレーム途中でR11を変えてもラッチ値は変わらない */
        set_crtc_reg(b, 11, 200);
        x68k_bus_crtc_raster_flush(b, lines, 64);
        assert(b->crtc.raster.tx_y_zero == 100);

        /* 次フレーム: 新しいR11=200がラッチされる */
        x68k_bus_crtc_raster_frame_start(b, cycles_per_frame, 512);
        assert(b->crtc.raster.y_latched == 0);
        x68k_bus_crtc_raster_flush(b, lines, 64);
        assert(b->crtc.raster.y_latched == 1);
        assert(b->crtc.raster.tx_y_zero == 200);
    }

    /* I: ラスターフラッシュ単独（CPU停止フレームで全行合成される保証） */
    setup_standard_crtc(b);
    {
        int cycles_per_frame = 10000000 / 60;
        x68k_bus_crtc_raster_frame_start(b, cycles_per_frame, 512);
        int lines[64];
        int total = x68k_bus_crtc_raster_flush(b, lines, 64);
        assert(total == 567);
        assert(b->crtc.raster.line == 566);
    }

    x68k_bus_destroy(b);
    printf("crtc_raster_test: OK\n");
    return 0;
}
