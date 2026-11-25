#include "bus/bus_internal.h"
#include "render/renderer.h"
#include "render/video_effects.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_r2_state(void) {
    static x68k_bus_t bus;
    static uint8_t crtc[0x10000];
    x68k_r2_state_t state;

    memset(&bus, 0, sizeof(bus));
    memset(crtc, 0, sizeof(crtc));
    bus.crtc_regs = crtc;

    crtc[0x2600] = 0x44; /* AH + B/P。EXON/H/P/G/G/G/Tは無視 */
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_HALF_PALETTE0);

    crtc[0x2600] = 0x1F; /* EXON + H/P + B/P + G/G + G/T */
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_HALF);
    assert(state.blend_second && state.blend_text);

    crtc[0x2600] = 0x1C; /* EXON + H/P + B/P、相手なし */
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_NORMAL);

    crtc[0x2600] = 0x14; /* EXON + B/P、H/P=0 */
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_SPECIAL_PRIORITY);

    crtc[0x2600] = 0x18; /* EXON + H/P、B/P=0（未定義） */
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_NORMAL);
}

static void test_mix(void) {
    /* G=31/R=1/B=2/I=0 と G=0/R=2/B=31/I=1。各成分独立・切り捨て。 */
    uint16_t front = (uint16_t)((31u << 11) | (1u << 6) | (2u << 1));
    uint16_t back = (uint16_t)((2u << 6) | (31u << 1) | 1u);
    assert(x68k_mix_grb555(front, back) == (uint16_t)((15u << 11) | (1u << 6) |
                                                        (16u << 1) | 1u));
    /* 背景が黒ならvcnMix1相当。前景Iは捨てる。 */
    assert(x68k_mix_grb555(0xFFFFu, 0x0000u) == 0x7BDEu);
}

static void test_raw_samplers(void) {
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    gvram_page_t pages[4];
    uint16_t gcode[4 * 2];
    uint8_t tcode[2];
    int16_t spcode[2];
    sprite_scene_t sc;

    memset(gvram, 0, sizeof(gvram));
    memset(pages, 0, sizeof(pages));
    pages[0].mask = 0x0F;
    pages[1].base_off = 0x80000;
    pages[1].mask = 0xFF;
    gvram[1] = 0x0A;
    gvram[0x80000 + 1] = 0xB7;
    memset(gcode, 0xA5, sizeof(gcode));
    x68k_sample_gvram_codes_line(gvram, gcode, 2, pages, 2, 0, 0);
    assert(gcode[0] == 0x0A && gcode[1] == 0);
    assert(gcode[2] == 0xB7 && gcode[3] == 0);
    /* npages以降は未定義。不要な4面ゼロクリアを行わない。 */

    memset(tvram, 0, sizeof(tvram));
    /* x=0のbit7へ T3..T0 = 1010 を置く。 */
    tvram[0x00000] = 0x00;
    tvram[0x20000] = 0x80;
    tvram[0x40000] = 0x00;
    tvram[0x60000] = 0x80;
    x68k_sample_tvram_codes_line(tvram, tcode, 2, 0, 0, 0);
    assert(tcode[0] == 0x0A && tcode[1] == 0);

    memset(sp, 0, sizeof(sp));
    memset(&sc, 0, sizeof(sc));
    sc.disp_on = 1;
    /* sprite #0: 表示座標(0,0)、PRW=1、色ブロック3、PCG#0。 */
    sp[0] = 0;
    sp[1] = 16;
    sp[2] = 0;
    sp[3] = 16;
    sp[4] = 0x03;
    sp[5] = 0x00;
    sp[7] = 1;
    /* PCG#0左上セルの先頭ドットをnibble 5にする。 */
    sp[SP_PCG_OFF] = 0x50;
    x68k_sample_sp_codes_line(sp, spcode, 2, &sc, 0);
    assert(spcode[0] == 0x35 && spcode[1] == -1);

    sc.disp_on = 0;
    x68k_sample_sp_codes_line(sp, spcode, 2, &sc, 0);
    assert(spcode[0] == -1 && spcode[1] == -1);
}

static void put_palette(uint8_t *regs, int off, int index, uint16_t color) {
    regs[off + index * 2] = (uint8_t)(color >> 8);
    regs[off + index * 2 + 1] = (uint8_t)color;
}

static void test_half_translucency_compose(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[1];
    const uint16_t red = 31u << 6;
    const uint16_t green = 31u << 11;
    const uint16_t blue = 31u << 1;

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    crtc[0x2401] = 0; /* VC R0 GrMM=0: 512dot・16色4ページ */
    crtc[0xE001] = 15;
    crtc[0x2500] = 0x24; /* SP > TX > GR、背面→前面はSP/TX/GR */
    crtc[0x2601] = 0x6F; /* SON + TON + GS4-0 */
    put_palette(crtc, 0x2000, 1, green);
    put_palette(crtc, 0x2000, 2, red);
    put_palette(crtc, 0x2200, 0, blue);
    put_palette(crtc, 0x2200, 1, blue);

    /* page0の奇数コード3をベースにし、page1はG/G用の奇数パレット1を選ぶ。 */
    gvram[1] = 3;
    gvram[0x80000 + 1] = 0;
    crtc[0x2600] = 0x1E; /* EXON + H/P + B/P + G/G */
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(red, green)));

    /* G/TはGR背面のTXだけを一度混ぜる。 */
    gvram[0x80000 + 1] = 0;
    tvram[0] = 0x80; /* x=0のT0を1にする */
    crtc[0x2600] = 0x1D; /* EXON + H/P + B/P + G/T */
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(red, blue)));

    /* AHは通常のG/T表示状態にかかわらずテキストパレット0と合成する。 */
    crtc[0x2600] = 0x44; /* AH + B/P */
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(red, blue)));
}

/* B/P=0（カラーLSB領域指定）のモードデコード */
static void test_r2_state_bp0(void) {
    static x68k_bus_t bus;
    static uint8_t crtc[0x10000];
    x68k_r2_state_t state;

    memset(&bus, 0, sizeof(bus));
    memset(crtc, 0, sizeof(crtc));
    bus.crtc_regs = crtc;

    /* $193F: mint.xのQキー終了・&half値。AH=0,EXON=1,H/P=1,B/P=0,G/G=0,G/T=1 */
    crtc[0x2600] = 0x19;
    crtc[0x2601] = 0x3F;
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_HALF);
    assert(state.lsb_source == X68K_R2_LSB_COLOR);
    assert(state.blend_text == 1 && state.blend_second == 0);

    /* $197F: &mion（SONが立つがeffectには影響しない） */
    crtc[0x2600] = 0x19;
    crtc[0x2601] = 0x7F;
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_HALF);
    assert(state.lsb_source == X68K_R2_LSB_COLOR);

    /* $1D3F: B/P=1版（既存回帰、lsb_sourceのみ異なる） */
    crtc[0x2600] = 0x1D;
    crtc[0x2601] = 0x3F;
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_HALF);
    assert(state.lsb_source == X68K_R2_LSB_PAGE);

    /* B/P=0 かつ G/G=1 -> NORMAL（_XHCG相当、スコープ外） */
    crtc[0x2600] = 0x1A; /* EXON+H/P+G/G, B/P=0, G/T=0 */
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_NORMAL);

    /* B/P=0 かつ H/P=0 -> NORMAL（_XWC相当、スコープ外） */
    crtc[0x2600] = 0x11; /* EXON, H/P=0, B/P=0, G/T=1 */
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_NORMAL);

    /* AH=1 かつ B/P=0 -> NORMAL（_A系、スコープ外） */
    crtc[0x2600] = 0x40;
    x68k_bus_get_r2_state(&bus, &state);
    assert(state.effect == X68K_R2_EFFECT_NORMAL);
}

/* 1024ドットは16色のみ。R20の色数指定（256色・65536色）にかかわらず16色として読まれ、
 * B/P=0・B/P=1ともに512ドット16色と同じ領域判定で動作すること。 */
static void test_r2_1024dot_16color(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[1];
    const uint16_t behind_color = 31u << 1; /* blue、text_palette[0] */
    const uint16_t code = 0x7BDFu;          /* 下位4bit=0xF（奇数）。1024ドットでは16色として読む */
    const uint16_t pal_odd = (uint16_t)((31u << 11) | 1u); /* パレット0xF: green、I=1 */
    const uint16_t pal_even = (uint16_t)(31u << 6);        /* パレット0xE: red、I=0 */

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    /* 1024ドット（bit2）に256色・65536色の指定（bit1-0=3）を重ねた値。1024ドットは
     * 16色のみのため、色数指定は無視されて16色として読まれることもあわせて検証する。 */
    crtc[0x2401] = 0x07;
    crtc[0x2501] = 0xE4; /* R1標準値: 象限とバンクが1対1 */
    crtc[0xE001] = 15;
    crtc[0x2500] = 0x24; /* GR > TX > SP、GRが最前面でTXは背面（behind経路） */
    crtc[0x2601] = 0x6F; /* SON+TON+GS4-0 */
    /* text_palette[0]はB/P=0テストの間は0のままにする。0にしておかないと、
     * 全画素透明(コード0)のTVRAMがx68k_render_tvram_line()の通常透過判定
     * （パレット生値!=0なら不透過）に引っかかり、NORMAL経路のGR画像を
     * 覆ってしまう（このテストが検証したいB/P=0ゲートとは無関係の副作用）。 */
    put_palette(crtc, 0x2000, 0xF, pal_odd);
    put_palette(crtc, 0x2000, 0xE, pal_even);
    put_palette(crtc, 0x2200, 0, behind_color);
    gvram[0] = (uint8_t)(code >> 8);
    gvram[1] = (uint8_t)code;

    /* B/P=0: pal_raw[0xE]のI=0なら領域外で、パレット0xFがそのまま出る（合成なし）。 */
    crtc[0x2600] = 0x19;
    crtc[0x2601] = 0x3F;
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(pal_odd));

    /* pal_raw[0xE]のIを1にすると領域内。表示色は偶数化しないcode=0xFのパレット色。 */
    put_palette(crtc, 0x2000, 0xE, (uint16_t)(pal_even | 1u));
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(pal_odd, behind_color)));

    /* B/P=1: codeのbit0（奇数）で領域内。ベース色は偶数化(0xF→0xE)されたパレット色。 */
    crtc[0x2600] = 0x1D;
    crtc[0x2601] = 0x3F;
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(pal_even, behind_color)));
}

/* 16色モードの領域判定。ページLSBではなくpal_raw[code&~1]の
 * Iビットで領域が決まり、表示色はcode自体（偶数化なし）を引くこと。 */
static void test_region_16color(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[1];
    const uint16_t pal2 = 31u << 6;    /* red、I=0 */
    const uint16_t pal3 = 31u << 11;   /* green */
    const uint16_t behind = 31u << 1;  /* blue、text_palette[0] */

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    crtc[0x2401] = 0; /* 512dot・16色4ページ */
    crtc[0xE001] = 15;
    crtc[0x2500] = 0x24; /* GR > TX > SP */
    crtc[0x2601] = 0x6F;
    put_palette(crtc, 0x2200, 0, behind);
    crtc[0x2600] = 0x19;
    crtc[0x2601] = 0x3F; /* B/P=0, G/T=1 */

    /* pal_raw[2]のI=0: 領域外、そのまま表示（合成なし） */
    gvram[1] = 2;
    put_palette(crtc, 0x2000, 2, pal2);
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(pal2));

    /* pal_raw[2]のIを1にすると領域内。表示色はcode(=2)自身のpal_raw[2]。 */
    put_palette(crtc, 0x2000, 2, pal2 | 1u);
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(pal2 | 1u, behind)));

    /* code=3でも領域判定はpal_raw[2]依存のまま（ページLSBでは決まらない）で領域内。
     * ただし表示色はcode自体=pal_raw[3]（偶数化しない）を引く。 */
    gvram[1] = 3;
    put_palette(crtc, 0x2000, 3, pal3);
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(pal3, behind)));
}

/* 256色モードの領域判定とベースページ選択。
 * 2ページ構成でcode==0のページがベースを奪わないこと（code!=0ガードの回帰）。 */
static void test_region_256color(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[1];
    const uint16_t behind = 31u << 1;
    const uint16_t page1_color = 31u << 11;

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    crtc[0x2401] = 1; /* 512dot・256色2ページ */
    crtc[0xE001] = 15;
    crtc[0x2500] = 0x24; /* GR > TX > SP */
    crtc[0x2601] = 0x6F;
    put_palette(crtc, 0x2200, 0, behind);
    crtc[0x2600] = 0x19;
    crtc[0x2601] = 0x3F; /* B/P=0, G/T=1 */

    /* コード$FE/$FFがpal_raw[$FE]のIビットで判定されること（インデックス上限付近）。 */
    gvram[1] = 0xFE;
    put_palette(crtc, 0x2000, 0xFE, (uint16_t)((31u << 6) | 1u));
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555((uint16_t)((31u << 6) | 1u), behind)));

    /* page0のコードが0（pal_raw[0]のIビットを1にしても）、page0はベースにならず
     * page1が選ばれること（code!=0ガード）。 */
    memset(gvram, 0, 0x100000);
    put_palette(crtc, 0x2000, 0, 1u); /* pal_raw[0] I=1 だが page0 code=0 */
    gvram[0x80000 + 1] = 1; /* page1 code=1 */
    put_palette(crtc, 0x2000, 1, page1_color);
    x68k_compose_line(&bus, fb, 1, 1, 0);
    /* page1のcode=1は奇数側インデックスなので領域判定はpal_raw[0]依存(I=1)=領域内。
     * 表示色はpal_raw[1]=page1_color。 */
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(page1_color, behind)));
}

/* 65536色の領域判定。code==0でも領域判定が行われ、
 * 領域内なら黒(0)の不透明画素になること（16/256色との違い）。 */
static void test_region_65536color(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[1];
    const uint16_t behind = 31u << 1;

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    crtc[0x2401] = 3; /* 512dot・65536色 */
    crtc[0xE001] = 15;
    crtc[0x2500] = 0x24; /* GR > TX > SP */
    crtc[0x2601] = 0x6F;
    put_palette(crtc, 0x2200, 0, behind);
    crtc[0x2600] = 0x19;
    crtc[0x2601] = 0x3F;
    /* pal_raw[0] = L[0]=0xAB, L[1]=0x01（下位パレット）
     * pal_raw[1] = H[0]=0xCD, H[1]=0x00（上位パレット）
     * resolve(0)    = H[0]<<8|L[0] = 0xCDAB（ベース色）
     * resolve(0|1)  = H[0]<<8|L[1] = 0xCD01（領域判定用、bit0=1で領域内） */
    put_palette(crtc, 0x2000, 0, 0xAB01u);
    put_palette(crtc, 0x2000, 1, 0xCD00u);

    /* code==0: 16/256色ならページ自体がベースにならないが、65536色は1ページのみ
     * なのでcode!=0ガードを通さず領域判定される。 */
    gvram[0] = 0x00;
    gvram[1] = 0x00; /* code = 0x0000 */
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(0xCDABu, behind)));
}

/* 「背面レイヤが存在するが透明」と「そもそも背面に無い」を
 * 区別すること。GRが最背面のときの合成相手はリテラル0（黒）、それ以外で背面レイヤが
 * 存在すれば透明でもtext_palette[0]になる。B/P=1・B/P=0共通のロジックなので
 * B/P=1（既存の分かりやすい奇数コード判定）で検証する。 */
static void test_behind_layer_existence(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[1];
    const uint16_t pal3 = 31u << 6;   /* red、奇数コード3用（常に領域内） */
    const uint16_t behind = 31u << 1; /* blue、text_palette[0] */

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    crtc[0x2401] = 0; /* 16色4ページ */
    crtc[0xE001] = 15;
    /* B/P=1は偶数化(code&~1)してパレット参照するため、pal_raw[2]に設定する
     * （pal_raw[3]ではない）。 */
    put_palette(crtc, 0x2000, 2, pal3);
    put_palette(crtc, 0x2200, 0, behind);
    gvram[1] = 3; /* page0 code=3（奇数、B/P=1で常に領域内） */
    crtc[0x2600] = 0x1D;
    crtc[0x2601] = 0x3F; /* EXON+H/P+B/P+G/T */

    /* TVRAM x=0 のコードを5にする（プレーン0とプレーン2のbit7を立てる）。
     * text_palette[5]は透明(#1-#3)/不透明を切り替えるために使う。
     * text_palette[0]==0だと「透明」自体が既存の透過判定（パレット生値==0）で
     * 成立しなくなる（コード0のままではbehind値を非ゼロにできない）ため、
     * コードを0以外にしてbehind値(text_palette[0])と独立に制御する。 */
    tvram[0x00000] = 0x80; /* T0 */
    tvram[0x40000] = 0x80; /* T2 */

    /* #1: GR最前面、背面にTXのみ（SON=0）、TX画素(code=5)は透明のまま。
     * → 背面レイヤが「存在するが透明」なのでtext_palette[0]。 */
    crtc[0x2500] = 0x24; /* GR>TX>SP */
    crtc[0x2601] = (uint8_t)(0x20u | 0x0Fu); /* TON=1,SON=0,GS4-0全ON */
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(pal3, behind)));

    /* #2: TON=0かつSON=0（背面に有効なレイヤが無い）→ リテラル0（黒）。 */
    crtc[0x2601] = 0x0Fu; /* TON=0,SON=0,GS4-0全ON */
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(pal3, 0)));

    /* #3: GRが最背面（R1上位バイト$06、mint.xのQキー終了時と同じ配置）
     * → forループ自体が回らずリテラル0（黒）。 */
    crtc[0x2500] = 0x06; /* SP>TX>GR、GRが最背面 */
    crtc[0x2601] = (uint8_t)(0x20u | 0x0Fu); /* TON=1、TXは透明のまま */
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(pal3, 0)));

    /* #4: TXが不透明（既存動作の回帰）。text_palette[5]をbehind(blue)とも
     * pal3(red)とも違うgreenにして、実際のTX色が使われることを区別できるようにする。 */
    const uint16_t text_color = 31u << 11; /* green */
    crtc[0x2500] = 0x24;
    crtc[0x2601] = (uint8_t)(0x20u | 0x0Fu);
    put_palette(crtc, 0x2200, 5, text_color);
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(pal3, text_color)));
}

/* mint.xのQキー終了相当のend-to-end。
 * R1=$06e4（GR最背面）、R2=$193F（B/P=0疑似半輝度）、65536色1ページで、
 * GVRAM全面がリテラル0と合成された値になること。&mioff相当の解除も確認する。 */
static void test_mint_end_to_end(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[1];
    const uint16_t code = 0x7BDFu; /* I=1（奇数）の65536色コード */

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    crtc[0x2401] = 3; /* 512dot・65536色 */
    crtc[0xE001] = 15;
    crtc[0x2500] = 0x06; /* R1上位バイト: SP>TX>GR、GR最背面（gvon.s:311 の$06e4） */
    crtc[0x2601] = 0x6F; /* SON+TON+GS4-0 */
    /* text_palette[0]は0のまま（GR最背面ではTXが最終合成でGRより前面に来るため、
     * 非ゼロにするとTVRAM全部0=コード0のTXが不透明扱いになりGRを覆ってしまう）。 */
    /* 恒等パレット（resolve()が素通しになる設定） */
    for (unsigned k = 0; k < 256; k += 2) {
        uint16_t v = (uint16_t)((k << 8) | (k + 1));
        put_palette(crtc, 0x2000, k, v);
        put_palette(crtc, 0x2000, k + 1, v);
    }
    gvram[0] = (uint8_t)(code >> 8);
    gvram[1] = (uint8_t)code;
    crtc[0x2600] = 0x19;
    crtc[0x2601] = 0x3F; /* R2=$193F、B/P=0疑似半輝度 */

    x68k_compose_line(&bus, fb, 1, 1, 0);
    /* GR最背面なのでbehindはリテラル0（黒）。codeは奇数=領域内、偶数化しない。 */
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(code, 0)));

    /* &mioff相当: 恒等パレットの各バイト値からIビット(LSB)だけを落とすと、
     * resolve(code|1)のLSBが常に0になり全画素が領域外になる。
     * code=0x7BDF（既に奇数）→ resolve = H[0x7B]<<8|L[0xDF] = 0x7A<<8|0xDE = 0x7ADE。 */
    for (unsigned k = 0; k < 256; k += 2) {
        uint16_t v = (uint16_t)(((k & ~1u) << 8) | ((k + 1u) & ~1u));
        put_palette(crtc, 0x2000, k, v);
        put_palette(crtc, 0x2000, k + 1, v);
    }
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(0x7ADEu));
}

/* MMDSP `-g2`（半透明）の再現。
 * R1=$21e4（TX > GR > SP、SP/BG面がGRの背面）、R2=$196f（EXON+H/P+B/P=0+G/T）、
 * 512ドット65536色1ページ。SP/BG面にキーボード相当の画素を置き、
 * グラフィックパレットの状態だけを輝度変更（GTONE_SET）に対応させて切り替える。
 *
 * - 通常輝度: MMDSPのトーンテーブルは下位パレットの奇数エントリにIビットを立てる
 *   ため領域内になり、グラフィック色とSP/BG色が50%合成される。
 * - 最低輝度: トーン0のテーブルは全ゼロ。領域外かつパレット出力0となり、
 *   グラフィックは透過してSP/BG面がそのまま出る（XEiJ TGS_XHCT準拠）。
 *   ここが黒塗りだったのが「最低輝度でキーボードが消える」不具合だった。 */
static void test_mmdsp_g2_translucent_64k(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[1];
    const uint16_t keyboard = (uint16_t)((31u << 11) | (31u << 6) | (31u << 1)); /* 白 */

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    crtc[0x2401] = 3;    /* VC R0: 512dot・65536色 */
    crtc[0xE001] = 15;   /* コントラスト最大 */
    crtc[0x29] = 0x05;   /* CRTC R20下位: HD=01(512)・VD=01(512)（
                          * x68k_bus_get_sprite_scene()からは参照されない。VC/CRTC
                          * 描画パス側の他の判定用にそのまま残す） */
    crtc[0x2500] = 0x21; /* R1上位バイト: TX > GR > SP（MMDSP SET_GMODEのmode2） */
    crtc[0x2501] = 0xE4;
    crtc[0x2600] = 0x19; /* R2 = $196f */
    crtc[0x2601] = 0x6F;

    /* SP/BG面: スプライト#0を(0,0)に置き、x=0の色コードを0x35にする。 */
    sp[0x0811] = 0x05; /* $EB0810: bit0=H-RES=1・bit2=V-RES=1（512×512、
                        * R20互換のビット配置に合わせた値） */
    sp[0x0808] = 0x02; /* $EB0808 bit9 DISP/CPU=1 */
    sp[0] = 0;
    sp[1] = 16; /* x座標 */
    sp[2] = 0;
    sp[3] = 16;    /* y座標 */
    sp[4] = 0x03;  /* PCG#0・色ブロック3 */
    sp[7] = 1;     /* PRW=1 */
    sp[SP_PCG_OFF] = 0x50;
    put_palette(crtc, 0x2200, 0x35, keyboard);

    gvram[0] = 0x00;
    gvram[1] = 0x00; /* code = 0x0000（MMDSPの背景グラフィック無しの部分） */

    /* 通常輝度相当: pal_raw[0]=$0001（L[0]=$00, L[1]=$01）、pal_raw[1]=$0000。
     * resolve(0)=0x0000（ベース色は黒）、resolve(0|1)=0x0001でLSB=1＝領域内。 */
    put_palette(crtc, 0x2000, 0, 0x0001u);
    put_palette(crtc, 0x2000, 1, 0x0000u);
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(x68k_mix_grb555(0x0000u, keyboard)));

    /* 最低輝度相当: グラフィックパレット全ゼロ。領域外＋パレット出力0で透過し、
     * SP/BG面の色がそのまま（100%の明るさで）出る。 */
    put_palette(crtc, 0x2000, 0, 0x0000u);
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(keyboard));

    /* 対照: パレット出力が非0なら従来どおり不透過でSP/BG面を覆う。
     * L[0]=$02 → resolve(0)=0x0002、resolve(1)=0x0000でLSB=0＝領域外。 */
    put_palette(crtc, 0x2000, 0, 0x0200u);
    x68k_compose_line(&bus, fb, 1, 1, 0);
    assert(fb[0] == grb555_to_argb(0x0002u));
}

static void test_special_priority_compose(void) {
    static x68k_bus_t bus;
    static uint8_t ram[0x100000];
    static uint8_t gvram[0x200000];
    static uint8_t tvram[0x080000];
    static uint8_t sp[0x10000];
    static uint8_t crtc[0x10000];
    uint32_t fb[2];
    const uint16_t red = 31u << 6;
    const uint16_t blue = 31u << 1;

    memset(&bus, 0, sizeof(bus));
    memset(ram, 0, sizeof(ram));
    memset(gvram, 0, sizeof(gvram));
    memset(tvram, 0, sizeof(tvram));
    memset(sp, 0, sizeof(sp));
    memset(crtc, 0, sizeof(crtc));
    bus.ram = ram;
    bus.gvram = gvram;
    bus.tvram = tvram;
    bus.sp_ram = sp;
    bus.crtc_regs = crtc;
    crtc[0x2401] = 0; /* VC R0 GrMM=0: 512dot・16色4ページ */
    crtc[0xE001] = 15;
    crtc[0x2500] = 0x21; /* TX > GR > SP、背面→前面はSP/GR/TX */
    crtc[0x2601] = 0x6F;
    put_palette(crtc, 0x2000, 2, red);
    put_palette(crtc, 0x2200, 1, blue);
    gvram[1] = 2; /* 偶数領域: TXの前面表示に負ける */
    gvram[3] = 3; /* 奇数領域: ベースだけがTX/SPより前へ昇格する */
    tvram[0] = 0xC0; /* x=0,1 のT0を1にする */
    crtc[0x2600] = 0x14; /* EXON + B/P、H/P=0 */

    x68k_compose_line(&bus, fb, 2, 1, 0);
    assert(fb[0] == grb555_to_argb(blue));
    assert(fb[1] == grb555_to_argb(red));
}

int main(void) {
    test_r2_state();
    test_mix();
    test_raw_samplers();
    test_half_translucency_compose();
    test_r2_state_bp0();
    test_r2_1024dot_16color();
    test_region_16color();
    test_region_256color();
    test_region_65536color();
    test_behind_layer_existence();
    test_mint_end_to_end();
    test_mmdsp_g2_translucent_64k();
    test_special_priority_compose();
    puts("video_r2_effects_test: ALL OK");
    return 0;
}
