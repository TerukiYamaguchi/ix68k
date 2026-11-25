/*
 * x68k_bus_get_display_size() / x68k_bus_get_dot_width_units() の単体テスト
 * bus/CPU非依存: x68k_bus_t をスタック上に置きcrtc_regsだけ実配列を差す。
 *
 * 検証データは ROM (IPLROMXV.DAT) の _CRTMOD テーブル($FF6928、1エントリ32バイト)、
 * XEiJ (CRTC.java ヘッダコメント、実機実測値付き) の
 * CRTMOD別 R00-R07/R20L/HRL 対応表と、otoko-ios/x68k/otoko.c の Init() が
 * 直接ポークする値（crtcdata[] = {69,6,19,51,567,5,40,552,0x0111}、
 * 0xe8e006 |= 0b10 で HRL セット）から採る。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

static void setup_bus(x68k_bus_t *bus, uint8_t *crtc) {
    memset(bus, 0, sizeof(*bus));
    bus->crtc_regs = crtc;
}

/* R00-R07（0xE80000-0xE8000F）と R20（0xE80028-29）、HRL（0xE8E007 bit1）を設定する */
static void set_crtc(uint8_t *crtc, uint16_t r00, uint16_t r01, uint16_t r02, uint16_t r03,
                      uint16_t r04, uint16_t r05, uint16_t r06, uint16_t r07, uint8_t r20_hi,
                      uint8_t r20_lo, int hrl) {
#define W(off, v)                                                                                \
    do {                                                                                         \
        crtc[(off)] = (uint8_t)((v) >> 8);                                                       \
        crtc[(off) + 1] = (uint8_t)(v);                                                          \
    } while (0)
    W(0x00, r00);
    W(0x02, r01);
    W(0x04, r02);
    W(0x06, r03);
    W(0x08, r04);
    W(0x0A, r05);
    W(0x0C, r06);
    W(0x0E, r07);
#undef W
    crtc[0x28] = r20_hi;
    crtc[0x29] = r20_lo;
    crtc[0xE007] = (uint8_t)(hrl ? 0x02 : 0x00);
}

int main(void) {
    static uint8_t crtc[0x10000];
    /* staticでBSSに置く: x68k_bus_tをスタックに直接置くとWindowsの既定スタック
     * サイズ(1MB)を超えてSTATUS_STACK_OVERFLOWになる（実測で判明）。 */
    static x68k_bus_t bus;
    int w, h;
    x68k_crtc_frame_timing_t timing;

    /* --- 標準 CRTMOD $0A（256x256, 256色x2, 31.5kHz）: 既存の回帰確認 --- */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    set_crtc(crtc, 0x002D, 0x0004, 0x0006, 0x0026, 0x0237, 0x0005, 0x0028, 0x0228, 0x01, 0x10, 0);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 256 && h == 256);
    assert(x68k_bus_get_dot_width_units(&bus) == 6); /* ウィンドウ幅 256*6/2 = 768 */

    /* --- 標準 CRTMOD $00（512x512, 16色x1, 31.5kHz） --- */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    set_crtc(crtc, 0x005B, 0x0009, 0x0011, 0x0051, 0x0237, 0x0005, 0x0028, 0x0228, 0x00, 0x15, 0);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 512 && h == 512);
    assert(x68k_bus_get_dot_width_units(&bus) == 3); /* ウィンドウ幅 512*3/2 = 768 */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(timing.valid);
    assert(timing.numerator == 24u * 92u * 568u);
    assert(timing.denominator == 69551990u);

    /* --- 標準 CRTMOD $01（512x512, 16色x1, 15.98kHz）: 38.864MHz系も同じ幅に揃う --- */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    set_crtc(crtc, 0x004B, 0x0003, 0x0005, 0x0045, 0x0103, 0x0002, 0x0010, 0x0100, 0x03, 0x05, 0);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 512 && h == 512);
    assert(x68k_bus_get_dot_width_units(&bus) == 3); /* ウィンドウ幅 512*3/2 = 768 */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(!timing.valid); /* HF=0/VD=01はインターレースで本テストの対象外 */

    /* --- 標準 CRTMOD $03（256x256, 16色x1, 15.98kHz） --- */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    set_crtc(crtc, 0x0025, 0x0001, 0x0000, 0x0020, 0x0103, 0x0002, 0x0010, 0x0100, 0x03, 0x00, 0);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 256 && h == 256);
    assert(x68k_bus_get_dot_width_units(&bus) == 6); /* ウィンドウ幅 256*6/2 = 768 */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(timing.valid);
    assert(timing.numerator == 64u * 38u * 260u);
    assert(timing.denominator == 38863630u);
    crtc[0x29] = 0x0C; /* HF=0/VD=11: 非公開インターレース */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(!timing.valid);

    /* --- 標準 CRTMOD $10/$18（768x512, 31.5kHz）: 正方形ドットの基準モード --- */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    set_crtc(crtc, 0x0089, 0x000E, 0x001C, 0x007C, 0x0237, 0x0005, 0x0028, 0x0228, 0x03, 0x16, 0);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 768 && h == 512);
    assert(x68k_bus_get_dot_width_units(&bus) == 2); /* ウィンドウ幅 768*2/2 = 768（基準） */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(timing.valid);
    assert(timing.numerator == 16u * 138u * 568u);
    assert(timing.denominator == 69551990u);

    /* --- 標準 CRTMOD $11（1024x424, 24.70kHz） --- */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    set_crtc(crtc, 0x00AF, 0x000F, 0x001F, 0x009F, 0x01D0, 0x0007, 0x0020, 0x01C8, 0x03, 0x16, 0);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 1024 && h == 424); /* R06/R07差分(424)をそのまま採用 */
    assert(x68k_bus_get_dot_width_units(&bus) == 2); /* ウィンドウ幅 1024*2/2 = 1024 */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(timing.valid);
    assert(timing.numerator == 16u * 176u * 465u);
    assert(timing.denominator == 69551990u);

    /* --- 標準 CRTMOD $12（1024x848, 24.70kHz インターレース） --- */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    set_crtc(crtc, 0x00AF, 0x000F, 0x001F, 0x009F, 0x01D0, 0x0007, 0x0020, 0x01C8, 0x03, 0x1A, 0);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 1024 && h == 848); /* R06/R07差分(424)をインターレースで2倍 */
    assert(x68k_bus_get_dot_width_units(&bus) == 2); /* ウィンドウ幅 1024*2/2 = 1024 */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(!timing.valid); /* インターレースは本テストの対象外 */

    /* --- otoko.c Init() の CRTC 直接プログラミング（俗称"384x256"正方形モード）---
     * crtcdata[9] = {69,6,19,51,567,5,40,552,0x0111}, HRL=1（0xe8e006 |= 0b10）。
     * 表示範囲は 256x256（HD=R03-R02=32カラム=256ドット）のまま、
     * ドットクロックだけ divisor 4（768系と同じ dup補正込みで正方形）になる。 */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    set_crtc(crtc, 69, 6, 19, 51, 567, 5, 40, 552, 0x01, 0x11, 1);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 256 && h == 256);
    assert(x68k_bus_get_dot_width_units(&bus) == 4); /* ウィンドウ幅 256*4/2 = 512 */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(timing.valid); /* HF=1/HRL=1を60Hzへ退行させない */
    assert(timing.numerator == 32u * 70u * 568u);
    assert(timing.denominator == 69551990u);

    /* --- R20 が完全に 0（起動直後、_CRTMOD 未実行）: 512x512 フォールバック --- */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    x68k_bus_get_display_size(&bus, &w, &h);
    assert(w == 512 && h == 512);
    assert(x68k_bus_get_dot_width_units(&bus) == 3); /* ウィンドウ幅 512*3/2 = 768 */
    x68k_bus_crtc_get_frame_timing(&bus, &timing);
    assert(!timing.valid);

    /* --- 色数・実画面サイズはビデオコントローラR0($E82400)のGrMM(bit2-0)で決まる ---
     * 0=512ドット16色 / 1=512ドット256色 / 3=512ドット65536色 / 4=1024ドット16色
     * （XEiJ VideoController.java REG1 のビット定義。ROM の IOCS $91=$FFB114 が
     * R20上位バイトと$E82400へ同じ値を書くため両者は常に一致する）。
     * mint.x のようにIOCSを経由せずレジスタを直接叩くソフトにも追従する必要があり、
     * IOCS _CRTMOD のワーク値 $93C から引いてはいけない。 */
    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    static const struct {
        uint8_t grmm;
        gvram_color_mode_t color;
        int real1024;
    } GRMM_CASES[] = {
        {0, GVRAM_COLOR_16, 0},    {1, GVRAM_COLOR_256, 0},   {3, GVRAM_COLOR_65536, 0},
        {4, GVRAM_COLOR_16, 1},    {5, GVRAM_COLOR_256, 1},   {7, GVRAM_COLOR_65536, 1},
        /* 未定義値 2/6 は bit1 を優先して65536色扱い（XEiJ準拠） */
        {2, GVRAM_COLOR_65536, 0}, {6, GVRAM_COLOR_65536, 1},
    };
    for (size_t i = 0; i < sizeof(GRMM_CASES) / sizeof(GRMM_CASES[0]); i++) {
        crtc[0x2401] = GRMM_CASES[i].grmm;
        assert(x68k_bus_get_gvram_color_mode(&bus) == GRMM_CASES[i].color);
        assert(x68k_bus_is_gvram_1024dot(&bus) == GRMM_CASES[i].real1024);
    }
    /* IOCS _CRTMOD のワーク値 $93C は判定に使わない。矛盾する値を書いても
     * VC R0 の側が勝つ（mint.x のレジスタ直書きに追従するための回帰確認）。 */
    static uint8_t ram[0x1000];
    bus.ram = ram;
    ram[0x093C] = 16;   /* CRTMOD 16 = 768x512 16色・実画面1024 */
    crtc[0x2401] = 3;   /* GrMM=3 = 512ドット65536色 */
    assert(x68k_bus_get_gvram_color_mode(&bus) == GVRAM_COLOR_65536);
    assert(!x68k_bus_is_gvram_1024dot(&bus));

    return 0;
}
