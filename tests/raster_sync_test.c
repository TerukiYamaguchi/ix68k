/*
 * ラスタ同期レンダリングの回帰テスト
 * （docs/ix68k-internals/design/crtc_design.md §6.3）。
 *
 * XSP_020（external/x68k_xsp）で実際に報告された2つの演出——スプライトダブラーと
 * テキストのラスタスクロール——が、CPUを一切動かさずに「ラスタ割り込みハンドラが
 * 途中でレジスタ/スプライトRAMを書き換えた」状況を模擬するだけで検証できることを
 * 示す。x68k_render_sp_line()/x68k_bus_get_tvram_scroll_x/y() の呼び出し合間に
 * 値を書き換える形で、決定論的・数ミリ秒でテストする（crtc_design.md §6.3の意図どおり）。
 */
#include "bus/bus_internal.h"
#include "render/renderer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define W 32
#define H 64

static void set_sprite(uint8_t *sp, int n, int xpos, int ypos, int color, int spat, int prw) {
    uint8_t *e = sp + n * 8;
    e[0] = (uint8_t)(xpos >> 8);
    e[1] = (uint8_t)xpos;
    e[2] = (uint8_t)(ypos >> 8);
    e[3] = (uint8_t)ypos;
    uint16_t w2 = (uint16_t)(((color & 0xF) << 8) | (spat & 0xFF));
    e[4] = (uint8_t)(w2 >> 8);
    e[5] = (uint8_t)w2;
    e[6] = 0;
    e[7] = (uint8_t)(prw & 0x3);
}

static void set_sprite_ypos(uint8_t *sp, int n, int ypos) {
    sp[n * 8 + 2] = (uint8_t)(ypos >> 8);
    sp[n * 8 + 3] = (uint8_t)ypos;
}

/* 16x16パタン pat_no の全ドットに同一ニブルを書き込む（sprite_render_test.cと同型） */
static void fill_pattern16(uint8_t *sp, int pat_no, uint8_t nibble) {
    uint32_t v = 0x11111111u * (uint32_t)(nibble & 0xF);
    for (int cell = 0; cell < 4; cell++) {
        uint8_t *lw = sp + SP_PCG_OFF + (size_t)(pat_no * 4 + cell) * 32u;
        for (int row = 0; row < 8; row++) {
            lw[row * 4 + 0] = (uint8_t)(v >> 24);
            lw[row * 4 + 1] = (uint8_t)(v >> 16);
            lw[row * 4 + 2] = (uint8_t)(v >> 8);
            lw[row * 4 + 3] = (uint8_t)v;
        }
    }
}

static void set_crtc_reg(x68k_bus_t *b, int reg_num, uint16_t val) {
    b->crtc_regs[reg_num * 2] = (uint8_t)(val >> 8);
    b->crtc_regs[reg_num * 2 + 1] = (uint8_t)(val & 0xFFu);
}

static void setup_standard_crtc(x68k_bus_t *b) {
    set_crtc_reg(b, 4, 566); /* lines_per_frame = R04+1 = 567 */
    set_crtc_reg(b, 5, 16);
    set_crtc_reg(b, 6, 40);  /* vdisp_start = 41 */
    set_crtc_reg(b, 7, 552); /* vdisp_lines = 512 */
}

static void write_palette(x68k_bus_t *b, uint32_t base, int index, uint16_t color) {
    x68k_bus_write8(b, base + (uint32_t)index * 2u, (uint8_t)(color >> 8));
    x68k_bus_write8(b, base + (uint32_t)index * 2u + 1u, (uint8_t)color);
}

static void opm_write(x68k_bus_t *b, uint8_t reg, uint8_t value) {
    x68k_bus_opm_reg_write8(b, 0x01u, reg);
    x68k_bus_opm_reg_write8(b, 0x03u, value);
}

/* main.c の1 CPU チャンクと同じ周辺機器順序で1フレームを進める。
 * render_lines は実際には合成対象となる行を拾うだけで、描画を省略した場合と
 * ラスター/音源状態が同一であることを比較するために使う。 */
static int run_frame(x68k_bus_t *b, int render_lines) {
    int lines[64];
    int rendered = 0;
    int remaining = 10000000 / 60;
    x68k_bus_crtc_raster_frame_start(b, remaining, 512);
    while (remaining > 0) {
        int chunk = remaining < 997 ? remaining : 997;
        x68k_bus_mfp_tick(b, chunk);
        int n = x68k_bus_crtc_raster_advance(b, chunk, lines, 64);
        if (render_lines) {
            for (int i = 0; i < n; i++)
                rendered += x68k_bus_crtc_raster_display_row(b, lines[i]) >= 0;
        }
        x68k_bus_opm_tick(b, chunk);
        remaining -= chunk;
    }
    return rendered;
}

int main(void) {
    /* A: スプライトダブラー — 1個のスプライトが同一フレーム内で上帯と下帯の
     *    両方に出る（ラスタ割り込みハンドラがYPOSを書き換える想定）。
     *    フレーム終端の一括合成(x68k_render_sp)では下帯にしか出ないことも対比する。 */
    {
        x68k_bus_t *bus = x68k_bus_create();
        assert(bus != NULL);
        bus->sp_ram[0x0808] = 0x02; /* $EB0808 bit9(DISP/CPU)=1 */

        uint16_t palette[256];
        memset(palette, 0, sizeof(palette));
        palette[(1 << 4) | 5] = 0x1234;
        fill_pattern16(bus->sp_ram, 0, 5);
        set_sprite(bus->sp_ram, 0, 16 + 8, 16 + 8, 1, 0, 1); /* x0=8, y0=8（上帯） */

        sprite_scene_t sc;
        assert(x68k_bus_get_sprite_scene(bus, &sc) == 0);
        uint32_t pal_argb[256];
        for (int i = 0; i < 256; i++)
            pal_argb[i] = grb555_to_argb(palette[i]);

        static uint32_t out[W * H];
        for (int i = 0; i < W * H; i++)
            out[i] = 0xFF000000u;
        for (int y = 0; y < H; y++) {
            if (y == 32) {
                /* ラスタ割り込みハンドラ相当: スプライト#0を下帯(y0=40)へ移動する */
                set_sprite_ypos(bus->sp_ram, 0, 16 + 8 + 32);
            }
            x68k_render_sp_line(bus->sp_ram, out + (size_t)y * W, W, &sc, pal_argb, y,
                                /*overlay=*/0);
        }
        assert(out[8 * W + 8] == pal_argb[(1 << 4) | 5]);  /* 上帯: 書き換え前の位置 */
        assert(out[40 * W + 8] == pal_argb[(1 << 4) | 5]); /* 下帯: 書き換え後の位置 */
        printf("raster_sync_test: A (スプライトダブラー、ライン単位即時反映) OK\n");

        /* 対比: 従来のフレーム一括合成では最終状態(下帯)しか見えない */
        set_sprite_ypos(bus->sp_ram, 0, 16 + 8); /* 上帯へ戻してから最終状態を下帯にする */
        set_sprite_ypos(bus->sp_ram, 0, 16 + 8 + 32);
        for (int i = 0; i < W * H; i++)
            out[i] = 0xFF000000u;
        x68k_render_sp(bus->sp_ram, out, W, H, &sc, palette, /*overlay=*/0);
        assert(out[8 * W + 8] == 0xFF000000u);             /* 上帯には出ない */
        assert(out[40 * W + 8] == pal_argb[(1 << 4) | 5]); /* 下帯にだけ出る */
        printf("raster_sync_test: A' (対比: 一括合成では上帯が消える) OK\n");

        x68k_bus_destroy(bus);
    }

    /* B: テキストX(R10)はライブ、Y(R11)はフレーム先頭ラッチ（crtc_design.md §6.2）。 */
    {
        x68k_bus_t *bus = x68k_bus_create();
        assert(bus != NULL);
        setup_standard_crtc(bus);
        set_crtc_reg(bus, 11, 100);

        int cycles_per_frame = 10000000 / 60;
        x68k_bus_crtc_raster_frame_start(bus, cycles_per_frame, 512);

        int lines[64];
        while (bus->crtc.raster.line < 41) {
            x68k_bus_crtc_raster_advance(bus, 50, lines, 64);
        }
        assert(x68k_bus_get_tvram_scroll_y(bus) == 100); /* vdisp_start到達でラッチ確定 */

        /* フレーム途中でR11を書き換えても、このフレームの合成には影響しない */
        set_crtc_reg(bus, 11, 200);
        assert(x68k_bus_get_tvram_scroll_y(bus) == 100);

        /* R10はライブ: 書き換えた瞬間に反映される */
        set_crtc_reg(bus, 10, 77);
        assert(x68k_bus_get_tvram_scroll_x(bus) == 77);
        set_crtc_reg(bus, 10, 5);
        assert(x68k_bus_get_tvram_scroll_x(bus) == 5);

        /* 次フレームでR11=200が新たにラッチされる */
        x68k_bus_crtc_raster_frame_start(bus, cycles_per_frame, 512);
        x68k_bus_crtc_raster_flush(bus, lines, 64);
        assert(x68k_bus_get_tvram_scroll_y(bus) == 200);
        printf("raster_sync_test: B (R10ライブ/R11ラッチ) OK\n");

        /* -raster-y live 相当: ラッチを無効化すると即座にライブ反映される */
        x68k_bus_set_raster_y_live(bus, 1);
        set_crtc_reg(bus, 11, 300);
        assert(x68k_bus_get_tvram_scroll_y(bus) == 300);
        printf("raster_sync_test: C (raster_y_live オプション) OK\n");

        x68k_bus_destroy(bus);
    }

    /* C: パレットはV-DISP開始時にラッチする。ラスターを回さない利用はliveへ
     * フォールバックし、-raster-pal liveでは表示中の書き換えも直ちに見える。 */
    {
        x68k_bus_t *bus = x68k_bus_create();
        assert(bus != NULL);
        setup_standard_crtc(bus);
        write_palette(bus, 0xE82000u, 3, 0x1234);
        write_palette(bus, 0xE82200u, 20, 0x2345);
        assert(x68k_bus_get_gfx_palette_raw(bus)[3] == 0x1234);
        assert(x68k_bus_get_text_palette_raw(bus)[20] == 0x2345);

        int lines[64], cycles_per_frame = 10000000 / 60;
        x68k_bus_crtc_raster_frame_start(bus, cycles_per_frame, 512);
        /* V-DISP前はsnapshot無しなのでlive値を返す。 */
        assert(x68k_bus_get_gfx_palette_raw(bus)[3] == 0x1234);
        while (bus->crtc.raster.line < 41)
            x68k_bus_crtc_raster_advance(bus, 50, lines, 64);
        write_palette(bus, 0xE82000u, 3, 0x3456);
        write_palette(bus, 0xE82200u, 20, 0x4567);
        assert(x68k_bus_get_gfx_palette_raw(bus)[3] == 0x1234);
        assert(x68k_bus_get_text_palette_raw(bus)[20] == 0x2345);

        x68k_bus_crtc_raster_frame_start(bus, cycles_per_frame, 512);
        x68k_bus_crtc_raster_flush(bus, lines, 64);
        assert(x68k_bus_get_gfx_palette_raw(bus)[3] == 0x3456);
        assert(x68k_bus_get_text_palette_raw(bus)[20] == 0x4567);
        x68k_bus_set_raster_pal_live(bus, 1);
        write_palette(bus, 0xE82000u, 3, 0x5678);
        write_palette(bus, 0xE82200u, 20, 0x6789);
        assert(x68k_bus_get_gfx_palette_raw(bus)[3] == 0x5678);
        assert(x68k_bus_get_text_palette_raw(bus)[20] == 0x6789);
        printf("raster_sync_test: C (palette V-DISP latch/live fallback) OK\n");
        x68k_bus_destroy(bus);
    }

    /* D: V-DISP(ch6)がラスター経由（垂直映像期間終了）で発火する
     *    （発火位置はフレーム末尾ではなくここ）。 */
    {
        x68k_bus_t *bus = x68k_bus_create();
        assert(bus != NULL);
        setup_standard_crtc(bus);
        bus->mfp.ierb |= 0x40u; /* IER-B bit6 = GPIP4/V-DISP enable */
        bus->mfp.imrb |= 0x40u;

        int cycles_per_frame = 10000000 / 60;
        x68k_bus_crtc_raster_frame_start(bus, cycles_per_frame, 512);
        assert(x68k_bus_crtc_raster_valid(bus) == 1);
        int lines[64];
        x68k_bus_crtc_raster_flush(bus, lines, 64);
        assert(mfp_ch_pending_test(&bus->mfp, MFP_CH_GPIP4));
        printf("raster_sync_test: D (V-DISP(ch6)のラスター経由発火) OK\n");

        x68k_bus_destroy(bus);
    }

    /* E: 描画を省略しても、CPUチャンクごとのラスタ走査・VDISP と OPM の進行は
     * 変わらない。render-lines 側は main.c が行う「合成対象行の判定」を模擬し、
     * skip 側ではその処理を完全に省く。PCM を同じ状態から生成して一致まで確認する。 */
    {
        x68k_bus_t *rendered = x68k_bus_create();
        x68k_bus_t *skipped = x68k_bus_create();
        assert(rendered != NULL && skipped != NULL);
        setup_standard_crtc(rendered);
        setup_standard_crtc(skipped);
        rendered->mfp.ierb |= 0x40u;
        rendered->mfp.imrb |= 0x40u;
        skipped->mfp.ierb |= 0x40u;
        skipped->mfp.imrb |= 0x40u;

        /* ym2151_gen_test と同じ、左右パン済みの単音。 */
        for (int which = 0; which < 2; which++) {
            x68k_bus_t *b = which ? skipped : rendered;
            opm_write(b, 0x20, 0xC7);
            opm_write(b, 0x60, 0x00);
            opm_write(b, 0x80, 0x1F);
            opm_write(b, 0x08, 0x08);
        }
        assert(run_frame(rendered, 1) == 512);
        assert(run_frame(skipped, 0) == 0);
        assert(rendered->crtc.raster.line == skipped->crtc.raster.line);
        assert(mfp_ch_pending_test(&rendered->mfp, MFP_CH_GPIP4));
        assert(mfp_ch_pending_test(&skipped->mfp, MFP_CH_GPIP4));

        int32_t pcm_rendered[256 * 2];
        int32_t pcm_skipped[256 * 2];
        x68k_bus_opm_generate(rendered, pcm_rendered, 256);
        x68k_bus_opm_generate(skipped, pcm_skipped, 256);
        assert(memcmp(pcm_rendered, pcm_skipped, sizeof(pcm_rendered)) == 0);
        printf("raster_sync_test: E (描画skip時のラスタ/VDISP/PCM不変性) OK\n");

        x68k_bus_destroy(rendered);
        x68k_bus_destroy(skipped);
    }

    printf("raster_sync_test: OK\n");
    return 0;
}
