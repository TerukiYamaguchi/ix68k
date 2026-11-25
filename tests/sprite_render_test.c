/*
 * x68k_render_sp() の単体テスト（スプライトのみ、BG無し、32個制限無し）。
 * docs/ix68k-internals/design/sprite_design.md §9 の描画・優先順位・モード検証に対応。
 * bus/CPU非依存: x68k_bus_t をスタック上に置きsp_ram/crtc_regsだけ実配列を差す。
 */
#include "bus/bus_internal.h"
#include "render/renderer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TW 64
#define TH 64

static void set_sprite(uint8_t *sp, int n, int xpos, int ypos, int vr, int hr, int color,
                       int spat, int prw) {
    uint8_t *e = sp + n * 8;
    e[0] = (uint8_t)(xpos >> 8);
    e[1] = (uint8_t)xpos;
    e[2] = (uint8_t)(ypos >> 8);
    e[3] = (uint8_t)ypos;
    uint16_t w2 = (uint16_t)((vr ? 0x8000 : 0) | (hr ? 0x4000 : 0) | ((color & 0xF) << 8) |
                              (spat & 0xFF));
    e[4] = (uint8_t)(w2 >> 8);
    e[5] = (uint8_t)w2;
    e[6] = 0;
    e[7] = (uint8_t)(prw & 0x3);
}

/* 16x16パタン pat_no の全ドットに同一ニブルを書き込む */
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

/* 8x8セル cell_no の全ドットに同一ニブルを書き込む */
static void fill_pattern8(uint8_t *sp, int cell_no, uint8_t nibble) {
    uint32_t v = 0x11111111u * (uint32_t)(nibble & 0xF);
    uint8_t *lw = sp + SP_PCG_OFF + (size_t)cell_no * 32u;
    for (int row = 0; row < 8; row++) {
        lw[row * 4 + 0] = (uint8_t)(v >> 24);
        lw[row * 4 + 1] = (uint8_t)(v >> 16);
        lw[row * 4 + 2] = (uint8_t)(v >> 8);
        lw[row * 4 + 3] = (uint8_t)v;
    }
}

/* BGデータエリア(base_off)の(row,col)（各0-63）エントリを設定する */
static void set_bgdata_entry(uint8_t *sp, int base_off, int row, int col, int vr, int hr,
                             int color, int pcgnum) {
    uint16_t entry = (uint16_t)((vr ? 0x8000 : 0) | (hr ? 0x4000 : 0) | ((color & 0xF) << 8) |
                                 (pcgnum & 0xFF));
    int off = base_off + row * 0x80 + col * 2;
    sp[off] = (uint8_t)(entry >> 8);
    sp[off + 1] = (uint8_t)entry;
}

/* $EB0810 解像度設定: CRTC R20下位バイトと同一配置。
 * bit0=H-RES(0=水平256/1=水平512)、bit1=R20 H-RESフィールド上位ビット
 * (1=768ドット/VGA系)、bit2=V-RES(0=垂直256/1=垂直512)、bit3=R20 V-RESフィールド
 * 上位ビット(1=1024ライン)、bit4=L/H(水平同期周波数)。 */
static void set_sp_reso_raw(uint8_t *sp, uint16_t val) {
    sp[0x0810] = (uint8_t)(val >> 8);
    sp[0x0811] = (uint8_t)val;
}

static void set_sp_resolution(uint8_t *sp, int hres, int vres) {
    set_sp_reso_raw(sp, (uint16_t)((hres ? 0x01 : 0) | (vres ? 0x04 : 0) | 0x10 /* L/H */));
}

/* CRTC R20 ($E80028) の表示モード: bit1-0=水平解像度(0=256/1=512/2=768)、
 * bit3-2=垂直解像度(0=256/1=512/2=1024) */
static void set_crtc_screen_mode(uint8_t *crtc, int h_reso, int v_reso) {
    crtc[0x28] = 0x00;
    crtc[0x29] = (uint8_t)(((v_reso & 3) << 2) | (h_reso & 3));
}

/* 16x16パタン pat_no の(px,py)（各0-15）ドット1個だけニブルをセットする */
static void set_pattern16_dot(uint8_t *sp, int pat_no, int px, int py, uint8_t nibble) {
    int sub = (px < 8 ? 0 : 2) + (py < 8 ? 0 : 1); /* TL/BL/TR/BR = 0/1/2/3 */
    int cell = pat_no * 4 + sub;
    int cx = px & 7, cy = py & 7;
    uint8_t *lw = sp + SP_PCG_OFF + (size_t)cell * 32u + (size_t)cy * 4u;
    uint32_t v = ((uint32_t)lw[0] << 24) | ((uint32_t)lw[1] << 16) | ((uint32_t)lw[2] << 8) |
                 lw[3];
    v &= ~(0xFu << ((7 - cx) * 4));
    v |= ((uint32_t)(nibble & 0xF)) << ((7 - cx) * 4);
    lw[0] = (uint8_t)(v >> 24);
    lw[1] = (uint8_t)(v >> 16);
    lw[2] = (uint8_t)(v >> 8);
    lw[3] = (uint8_t)v;
}

static void setup_bus(x68k_bus_t *bus, uint8_t *sp, uint8_t *crtc) {
    memset(bus, 0, sizeof(*bus));
    bus->sp_ram = sp;
    bus->crtc_regs = crtc;
    sp[0x0808] = 0x02; /* $EB0808 bit9(DISP/CPU)=1: スプライト/BG表示ON */
    sp[0x0809] = 0x00;
}

int main(void) {
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x3000];
    /* staticでBSSに置く: x68k_bus_tをスタックに直接置くとWindowsの既定スタック
     * サイズ(1MB)を超えてSTATUS_STACK_OVERFLOWになる（実測で判明）。 */
    static x68k_bus_t bus;
    static uint32_t out[TW * TH];
    uint16_t palette[256];
    sprite_scene_t sc;

    /* --- A: PCGデコード（フルレンダ経由の間接検証）--- */
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, sp, crtc);
    memset(palette, 0, sizeof(palette));
    palette[(3 << 4) | 7] = 0x1234;
    set_pattern16_dot(sp, 0, 0, 0, 7); /* パタン0の左上(0,0)だけニブル7 */
    set_sprite(sp, 0, 16, 16, 0, 0, 3, 0, 1);
    assert(x68k_bus_get_sprite_scene(&bus, &sc) == 0);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == grb555_to_argb(0x1234));   /* (0,0): パタン(0,0)のニブル7 */
    assert(out[1] == 0xFF000000u);              /* (1,0): 透過→overlay=0で黒 */
    printf("sprite_render_test: A (PCGデコード) OK\n");

    /* --- B: スプライト単体+(16,16)オフセット --- */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    memset(palette, 0, sizeof(palette));
    palette[(2 << 4) | 5] = 0x5678;
    fill_pattern16(sp, 0, 5);
    set_sprite(sp, 0, 16, 16, 0, 0, 2, 0, 1); /* XPOS=YPOS=16 → 実画面(0,0)起点 */
    x68k_bus_get_sprite_scene(&bus, &sc);
    memset(out, 0xAB, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/1);
    assert(out[0] == grb555_to_argb(0x5678));           /* (0,0): スプライト左上 */
    assert(out[15 * TW + 15] == grb555_to_argb(0x5678)); /* (15,15): スプライト右下 */
    assert(out[16] == 0xABABABABu); /* (16,0): スプライト範囲外、overlay=1で下地保持 */
    printf("sprite_render_test: B (座標オフセット) OK\n");

    /* --- C: 透過（overlay=1で下地保持）--- */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    memset(palette, 0, sizeof(palette));
    palette[(1 << 4) | 9] = 0x0F0F;
    set_pattern16_dot(sp, 0, 3, 3, 9); /* パタン内(3,3)だけ不透過、他は0(透過) */
    set_sprite(sp, 0, 16, 16, 0, 0, 1, 0, 1);
    x68k_bus_get_sprite_scene(&bus, &sc);
    memset(out, 0xCD, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/1);
    assert(out[3 * TW + 3] == grb555_to_argb(0x0F0F)); /* (3,3): 不透過ドット */
    assert(out[0] == 0xCDCDCDCDu);                     /* (0,0): 透過→下地保持 */
    assert(out[4 * TW + 4] == 0xCDCDCDCDu);             /* (4,4): 透過→下地保持 */
    printf("sprite_render_test: C (透過) OK\n");

    /* --- D: 反転（HR/VRで鏡像）--- */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    memset(palette, 0, sizeof(palette));
    palette[(0 << 4) | 8] = 0x2222;
    set_pattern16_dot(sp, 0, 0, 0, 8); /* パタン内(0,0)=左上だけ不透過 */
    set_sprite(sp, 0, 16, 16, /*vr=*/0, /*hr=*/1, 0, 0, 1); /* 水平反転のみ */
    x68k_bus_get_sprite_scene(&bus, &sc);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[15] == grb555_to_argb(0x2222)); /* (15,0): 左上ドットが右上へ反転 */
    assert(out[0] == 0xFF000000u);             /* (0,0): 反転後は透過 */

    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    set_pattern16_dot(sp, 0, 0, 0, 8);
    set_sprite(sp, 0, 16, 16, /*vr=*/1, /*hr=*/0, 0, 0, 1); /* 垂直反転のみ */
    x68k_bus_get_sprite_scene(&bus, &sc);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[15 * TW + 0] == grb555_to_argb(0x2222)); /* (0,15): 左上ドットが左下へ反転 */
    printf("sprite_render_test: D (反転) OK\n");

    /* --- F: スプライト間優先（低番号が手前）--- */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    memset(palette, 0, sizeof(palette));
    palette[(4 << 4) | 1] = 0x1111; /* スプライト#0の色 */
    palette[(5 << 4) | 1] = 0x2222; /* スプライト#1の色 */
    fill_pattern16(sp, 0, 1);
    set_sprite(sp, 1, 16, 16, 0, 0, 5, 0, 1); /* 先に置く(高番号) */
    set_sprite(sp, 0, 16, 16, 0, 0, 4, 0, 1); /* 同じ位置、低番号 */
    x68k_bus_get_sprite_scene(&bus, &sc);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == grb555_to_argb(0x1111)); /* 低番号が手前に表示される */
    printf("sprite_render_test: F (スプライト間優先) OK\n");

    /* --- E: BGタイリング+球面スクロール --- */
    /* E1: 512モード(pat_size=16, real_wh=1024, BG0)の基本タイリング */
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, sp, crtc);
    set_sp_resolution(sp, /*hres=*/1, /*vres=*/1); /* $EB0810: 512x512モード */
    memset(palette, 0, sizeof(palette));
    palette[(6 << 4) | 3] = 0x3333;
    fill_pattern16(sp, 5, 3);
    set_bgdata_entry(sp, SP_BGDATA0_OFF, 0, 0, 0, 0, 6, 5);
    sp[0x0809] |= 0x01; /* BG0 ON (bit0) */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) == 0);
    assert(sc.pat_size == 16 && sc.real_wh == 1024 && sc.bg0_on);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == grb555_to_argb(0x3333)); /* (0,0): BG0タイル(row0,col0) */

    /* E2: 512モードの球面ラップ（BG0スクロールで実画面外周のcol=63が(0,0)に来る） */
    palette[(7 << 4) | 4] = 0x4444;
    fill_pattern16(sp, 6, 4);
    set_bgdata_entry(sp, SP_BGDATA0_OFF, 0, 63, 0, 0, 7, 6); /* col=63(実画面右端) */
    sp[0x0800] = (uint8_t)(1008 >> 8);
    sp[0x0801] = (uint8_t)1008; /* BG0 XPOS=1008=real_wh-16 */
    x68k_bus_get_sprite_scene(&bus, &sc);
    assert(sc.bg0_sx == 1008);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == grb555_to_argb(0x4444)); /* 球面ラップでcol=63が出力(0,0)に来る */

    /* E3: 256モード(pat_size=8, real_wh=512, BG1)の基本タイリング */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    set_sp_resolution(sp, /*hres=*/0, /*vres=*/0); /* $EB0810: 256x256モード */
    memset(palette, 0, sizeof(palette));
    palette[(1 << 4) | 2] = 0x5555;
    fill_pattern8(sp, 10, 2);
    set_bgdata_entry(sp, SP_BGDATA1_OFF, 0, 0, 0, 0, 1, 10);
    sp[0x0809] |= 0x08; /* BG1 ON (bit3) */
    sp[0x0809] |= 0x10; /* BG1 TXSEL bit4=1: BGデータエリア1を使用 */
    x68k_bus_get_sprite_scene(&bus, &sc);
    assert(sc.pat_size == 8 && sc.real_wh == 512 && sc.two_bg_pages && sc.bg1_on);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == grb555_to_argb(0x5555)); /* (0,0): BG1タイル(row0,col0) */

    /* E4: 256モードの球面ラップ（BG1スクロールで実画面外周のrow=63が(0,0)に来る） */
    palette[(2 << 4) | 9] = 0x6666;
    fill_pattern8(sp, 11, 9);
    set_bgdata_entry(sp, SP_BGDATA1_OFF, 63, 0, 0, 0, 2, 11); /* row=63(実画面下端) */
    sp[0x0806] = (uint8_t)(504 >> 8);
    sp[0x0807] = (uint8_t)504; /* BG1 YPOS=504=real_wh-8 */
    x68k_bus_get_sprite_scene(&bus, &sc);
    assert(sc.bg1_sy == 504);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == grb555_to_argb(0x6666)); /* 球面ラップでrow=63が出力(0,0)に来る */
    printf("sprite_render_test: E (BGタイリング+球面スクロール) OK\n");

    /* --- G: PRW×R1優先順位（x68k_bus_get_screen_order）--- */
    {
        static x68k_bus_t b2; /* スタックオーバーフロー対策（上記bus同様） */
        static uint8_t crtc2[0x3000];
        memset(&b2, 0, sizeof(b2));
        memset(crtc2, 0, sizeof(crtc2));
        b2.crtc_regs = crtc2;
        int order[3];

        /* SP最優先(00) > TX(01) > GR(10) → 背面から GR,TX,SP（SPが最前面） */
        crtc2[0x2500] = (uint8_t)((0 << 4) | (1 << 2) | 2);
        x68k_bus_get_screen_order(&b2, order);
        assert(order[0] == 0 && order[1] == 1 && order[2] == 2);

        /* GR最優先(00) > SP(01) > TX(10) → 背面から TX,SP,GR（GRが最前面） */
        crtc2[0x2500] = (uint8_t)((1 << 4) | (2 << 2) | 0);
        x68k_bus_get_screen_order(&b2, order);
        assert(order[0] == 1 && order[1] == 2 && order[2] == 0);

        /* デフォルト(全0、同値・本来禁止設定) → 既存互換でTXが最前面 */
        crtc2[0x2500] = 0x00;
        x68k_bus_get_screen_order(&b2, order);
        assert(order[2] == 1);
    }
    printf("sprite_render_test: G (PRW×R1優先順位) OK\n");

    /* --- H: 512×256/256×512混在モードは正規サポート。
     * 旧実装はH-RES≠V-RESを一律非対応としていたが、$EB0810のビット配置が
     * R20下位バイト互換だと判明したため撤回。混在モードのBG面数はH-RESのみで
     * 決まる（V-RESは無関係）。 */
    /* H-1: hres=1,vres=0（512×256、pat_size=16/real_wh=1024/two_bg_pages=0、
     * BG0のみ。BG1 ONを立ててもtwo_bg_pages=0のため無視される） */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    set_sp_resolution(sp, /*hres=*/1, /*vres=*/0);
    memset(palette, 0, sizeof(palette));
    palette[(1 << 4) | 2] = 0x9999; /* BG1用パレット。two_bg_pages=0なので描かれないはず */
    fill_pattern16(sp, 3, 2);
    set_bgdata_entry(sp, SP_BGDATA1_OFF, 0, 0, 0, 0, 1, 3);
    sp[0x0809] |= 0x08; /* BG1 ON (bit3) */
    sp[0x0809] |= 0x10; /* BG1 TXSEL bit4=1: BGデータエリア1を使用 */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) == 0);
    assert(sc.disp_on == 1 && sc.pat_size == 16 && sc.real_wh == 1024 && sc.two_bg_pages == 0);
    assert(sc.bg1_on == 0); /* two_bg_pages=0のためBG1 ONビットは無視される */
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == 0xFF000000u); /* BG1が無視されるため透過(黒) */

    /* H-2: hres=0,vres=1（256×512、pat_size=8/real_wh=512/two_bg_pages=1、
     * BG1有効。BG1 ONを立てると実際に描画される） */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    set_sp_resolution(sp, /*hres=*/0, /*vres=*/1);
    memset(palette, 0, sizeof(palette));
    palette[(1 << 4) | 2] = 0xAAAA;
    fill_pattern8(sp, 3, 2);
    set_bgdata_entry(sp, SP_BGDATA1_OFF, 0, 0, 0, 0, 1, 3);
    sp[0x0809] |= 0x08; /* BG1 ON (bit3) */
    sp[0x0809] |= 0x10; /* BG1 TXSEL bit4=1: BGデータエリア1を使用 */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) == 0);
    assert(sc.disp_on == 1 && sc.pat_size == 8 && sc.real_wh == 512 && sc.two_bg_pages == 1);
    assert(sc.bg1_on == 1);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == grb555_to_argb(0xAAAA)); /* BG1タイルが実際に描かれる */

    /* H-3: bit1/bit3（R20のH-RES/V-RESフィールド上位ビット、768ドット系/1024ライン系）
     * が立つ組み合わせは非対応 */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    set_sp_reso_raw(sp, 0x02); /* H field=2(768系) */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) != 0);
    assert(sc.disp_on == 0);
    set_sp_reso_raw(sp, 0x08); /* V field=2(1024ライン系) */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) != 0);
    assert(sc.disp_on == 0);
    set_sp_reso_raw(sp, 0x16); /* 規約どおりの768x512値 */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) != 0);
    assert(sc.disp_on == 0);
    set_sp_reso_raw(sp, 0xFF); /* IPLROM mode16実測値 */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) != 0);
    assert(sc.disp_on == 0);
    printf("sprite_render_test: H (512x256/256x512は正規サポート、768ドット系等は非対応) OK\n");

    /* --- I: 同一水平線32個制限（33個目は非表示）--- */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    memset(palette, 0, sizeof(palette));
    palette[(3 << 4) | 1] = 0x7777; /* #0-31（32個）の色 */
    palette[(4 << 4) | 1] = 0x8888; /* #32（33個目）の色 */
    fill_pattern16(sp, 0, 1);
    for (int n = 0; n < 32; n++)
        set_sprite(sp, n, 16, 16, 0, 0, 3, 0, 1);  /* #0-31: 同じ位置(実画面0,0) */
    set_sprite(sp, 32, 32, 16, 0, 0, 4, 0, 1);     /* #32: 別位置(実画面16,0) */
    x68k_bus_get_sprite_scene(&bus, &sc);
    memset(out, 0, sizeof(out));
    x68k_render_sp(bus.sp_ram, out, TW, TH, &sc, palette, /*overlay=*/0);
    assert(out[0] == grb555_to_argb(0x7777));  /* (0,0): 32個以内なので表示される */
    assert(out[16] == 0xFF000000u);            /* (16,0): 33個目は非表示→透過(黒) */
    printf("sprite_render_test: I (同一水平線32個制限) OK\n");

    /* --- J: 判定は$EB0810のみで行い、CRTC R20は無関係であることの契約テスト。
     *旧実装はCRTC R20の解像度ビットでも事前ゲートしていたが、
     * CRTC直叩きの正方形ドットモードではR20と$EB0810が食い違うため撤廃した。
     * CRTC R20を768x512のまま固定し、$EB0810だけを変えて判定がCRTCの影響を
     * 受けないことを確認する。MMDSP終了後の768x512残骸は、_CRTMODが$EB0810に
     * $FFを書くこと（IPLROM mode16実測値）で自然に解消する（sprite_design.md §9の受入条件は
     * 実機での別途確認が必要）。 */
    memset(sp, 0, sizeof(sp));
    setup_bus(&bus, sp, crtc);
    set_crtc_screen_mode(crtc, /*h_reso=*/2, /*v_reso=*/1); /* CRTC表示画面は768x512に固定 */
    set_sp_reso_raw(sp, 0xFF); /* J-1: IPLROM mode16実測値 */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) != 0);
    assert(sc.disp_on == 0);
    set_sp_reso_raw(sp, 0x16); /* J-2: 規約どおりの768x512値 */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) != 0);
    assert(sc.disp_on == 0);
    set_sp_reso_raw(sp, 0x10); /* J-3: CRTCは768x512のままでも$EB0810が256x256なら表示される */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) == 0);
    assert(sc.disp_on == 1);
    printf("sprite_render_test: J ($EB0810のみで判定、CRTC R20は無関係) OK\n");

    /* --- K: OTOKO.X回帰テスト。CRTC直叩きの正方形ドット
     * モードでR20とスプライトコントローラの設定が食い違うのが正常。CRTC側の
     * 値を判定に使うとこのケースが落ちる。実測値はMCPでOTOKO.Xタイトル画面
     * から直接読み取った値。 */
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc)); /* [0x3000] 実配列サイズはmain()冒頭のstatic宣言参照 */
    setup_bus(&bus, sp, crtc);
    crtc[0x28] = 0x03;
    crtc[0x29] = 0x11; /* R20 = $0311: H-RESフィールド=1(512系分周比)、V-RESフィールド=0(256) */
    crtc[0x04] = 0x00;
    crtc[0x05] = 0x13;
    crtc[0x06] = 0x00;
    crtc[0x07] = 0x33; /* R02/R03: 実カラム数256 */
    set_sp_reso_raw(sp, 0x10); /* $EB0810 = 0x10（_CRTMOD mode2が書いた値のまま） */
    assert(x68k_bus_get_sprite_scene(&bus, &sc) == 0);
    assert(sc.disp_on == 1 && sc.pat_size == 8 && sc.real_wh == 512 && sc.two_bg_pages == 1);
    printf("sprite_render_test: K (OTOKO.X回帰、CRTC R20非依存) OK\n");

    /* --- L: IPLROMモードテーブル駆動テスト。$FF65E0のモード
     * テーブル+0x17バイト実測値を網羅する。L/Hビット(bit4)はBGパタンサイズ・
     * 実画面サイズ・BG面数の判定に影響しないことも確認する（0x15と0x05）。 */
    {
        static const struct {
            uint8_t reso;
            int valid;
            int pat_size;
            int real_wh;
            int two_bg_pages;
        } ltab[] = {
            {0x15, 1, 16, 1024, 0}, /* mode0/4/8/12 512x512 */
            {0x05, 1, 16, 1024, 0}, /* mode1/5/9/13 512x512標準(L/H=0) */
            {0x10, 1, 8, 512, 1},   /* mode2/6/10/14 256x256 */
            {0x00, 1, 8, 512, 1},   /* mode3/7/11/15 256x256標準 */
            {0xFF, 0, 0, 0, 0},     /* mode16/17/18 768x512・1024系 */
        };
        for (size_t i = 0; i < sizeof(ltab) / sizeof(ltab[0]); i++) {
            memset(sp, 0, sizeof(sp));
            setup_bus(&bus, sp, crtc);
            set_sp_reso_raw(sp, ltab[i].reso);
            int rc = x68k_bus_get_sprite_scene(&bus, &sc);
            if (ltab[i].valid) {
                assert(rc == 0);
                assert(sc.pat_size == ltab[i].pat_size);
                assert(sc.real_wh == ltab[i].real_wh);
                assert(sc.two_bg_pages == ltab[i].two_bg_pages);
            } else {
                assert(rc != 0);
                assert(sc.disp_on == 0);
            }
        }
    }
    printf("sprite_render_test: L (IPLROMモードテーブル網羅) OK\n");

    printf("sprite_render_test: ALL OK\n");
    return 0;
}
