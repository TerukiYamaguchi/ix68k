/*
 * グラフィック画面65536色モードのパレット変換の回帰テスト。
 *
 * 『Inside X68000』3-6-2節: $E82000-$E821FF の512バイトは、16/256色モードでは
 * 256エントリ×GRB555の単純なパレットだが、65536色モードでは「8bit×256×2組」
 * として再解釈される。下位パレット($E82000から4バイトおき)が色コード下位8bitを、
 * 上位パレット($E82002から4バイトおき)が色コード上位8bitを変換し、連結して最終的な
 * GRB555になる。設計資料: docs/ix68k-internals/design/video_controller_design.md §4
 */
#include "render/renderer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GVRAM_BYTES 0x200000

/* 7.1-a: pal_raw[]からのアドレス写像がvideo_controller_design.md §4の期待値と一致することを確認 */
static void test_address_mapping(void) {
    uint16_t pal_raw[256];

    /* pal_raw[0] = 0xABCD: 上位バイトAB→L[0], 下位バイトCD→L[1] */
    memset(pal_raw, 0, sizeof(pal_raw));
    pal_raw[0] = 0xABCD;
    assert(x68k_gfx_pal64k_lo(pal_raw, 0) == 0xAB);
    assert(x68k_gfx_pal64k_lo(pal_raw, 1) == 0xCD);

    /* pal_raw[1] = 0xEF12: 上位バイトEF→H[0], 下位バイト12→H[1] */
    pal_raw[1] = 0xEF12;
    assert(x68k_gfx_pal64k_hi(pal_raw, 0) == 0xEF);
    assert(x68k_gfx_pal64k_hi(pal_raw, 1) == 0x12);

    /* 色コード0x0000 = (H[0]<<8)|L[0] = 0xEF00|0xAB = 0xEFAB */
    assert(x68k_gfx_pal64k_resolve(pal_raw, 0x0000) == 0xEFAB);
    /* 色コード0x0101 = (H[1]<<8)|L[1] = 0x1200|0xCD = 0x12CD */
    assert(x68k_gfx_pal64k_resolve(pal_raw, 0x0101) == 0x12CD);
}

/* 7.1-b: 恒等パレット（改修前の「パレット無視」挙動と一致する構成）では素通しになる */
static void test_identity(void) {
    uint16_t pal_raw[256];

    for (unsigned k = 0; k < 256; k += 2) {
        pal_raw[k] = (uint16_t)((k << 8) | (k + 1));
        pal_raw[k + 1] = (uint16_t)((k << 8) | (k + 1));
    }
    for (unsigned code = 0; code <= 0xFFFF; code += 0x1111) {
        assert(x68k_gfx_pal64k_resolve(pal_raw, (uint16_t)code) == (uint16_t)code);
    }
    assert(x68k_gfx_pal64k_resolve(pal_raw, 0xFFFF) == 0xFFFF);
}

/* 7.1-c: 輝度ダウン回帰。各成分を半分にスケールしたパレットで暗くなる */
static void test_brightness_down(void) {
    uint16_t pal_full[256];
    uint16_t pal_half[256];

    /* 恒等パレット(フル輝度) */
    for (unsigned k = 0; k < 256; k += 2) {
        pal_full[k] = (uint16_t)((k << 8) | (k + 1));
        pal_full[k + 1] = (uint16_t)((k << 8) | (k + 1));
    }
    /* mint set_gtone_64k同様、恒等変換の各バイト値kをk/2にスケールしたパレット
     * （L[k]=H[k]=k/2）。素朴に暗くなることだけを確認するのが目的なので、
     * GRB555の各フィールド境界には合わせない。 */
    for (unsigned k = 0; k < 256; k += 2) {
        uint8_t fk = (uint8_t)(k / 2);
        uint8_t fk1 = (uint8_t)((k + 1) / 2);
        pal_half[k] = (uint16_t)((fk << 8) | fk1);
        pal_half[k + 1] = (uint16_t)((fk << 8) | fk1);
    }

    uint16_t code = 0xFFFF; /* 全bit1。full解決値0xFFFF・half解決値0x7F7Fは手計算で
                              * G=31→15, R=31→29, B=31→31(不変)と検証済み。 */
    uint16_t full_grb = x68k_gfx_pal64k_resolve(pal_full, code);
    uint16_t half_grb = x68k_gfx_pal64k_resolve(pal_half, code);

    /* grb555_to_argb()はIビットを無視するため、R/G/Bの5bitフィールドだけを比較する */
    unsigned full_r = (full_grb >> 6) & 0x1Fu, half_r = (half_grb >> 6) & 0x1Fu;
    unsigned full_g = (full_grb >> 11) & 0x1Fu, half_g = (half_grb >> 11) & 0x1Fu;
    unsigned full_b = (full_grb >> 1) & 0x1Fu, half_b = (half_grb >> 1) & 0x1Fu;
    assert(half_r <= full_r && half_g <= full_g && half_b <= full_b);
    assert(half_r < full_r || half_g < full_g || half_b < full_b);
}

/* 7.1-d: 全GVRAM経由の1行描画。ビッグエンディアン16bit値が正しく変換されること */
static void test_render_line(void) {
    static uint8_t gvram[GVRAM_BYTES];
    gvram_page_t page;
    uint32_t pal_argb[256]; /* 65536色では未使用 */
    uint16_t pal_raw[256];
    uint32_t out_line[4];

    memset(gvram, 0, sizeof(gvram));
    memset(pal_argb, 0, sizeof(pal_argb));
    memset(&page, 0, sizeof(page));
    page.mask = 0;
    page.visible = 1;

    for (unsigned k = 0; k < 256; k += 2) {
        pal_raw[k] = (uint16_t)((k << 8) | (k + 1));
        pal_raw[k + 1] = (uint16_t)((k << 8) | (k + 1));
    }

    /* x=0にGRB555=0x7BDE（実質の恒等変換なのでそのまま返るはず）をビッグエンディアンで置く */
    gvram[0] = 0x7B;
    gvram[1] = 0xDE;

    x68k_render_gvram_line(gvram, out_line, 1, &page, 1, pal_argb, pal_raw, 0, 0, 0);
    assert(out_line[0] == grb555_to_argb(0x7BDE));
}

/* 7.1-e: visible=0のときは従来どおり透過（gvram_renderer.c:47-51の分岐固定） */
static void test_invisible_transparent(void) {
    static uint8_t gvram[GVRAM_BYTES];
    gvram_page_t page;
    uint32_t pal_argb[256];
    uint16_t pal_raw[256];
    uint32_t out_line[1];

    memset(gvram, 0xFF, sizeof(gvram)); /* 非0で埋め、読まれたら検出できるようにする */
    memset(pal_argb, 0, sizeof(pal_argb));
    memset(pal_raw, 0, sizeof(pal_raw));
    memset(&page, 0, sizeof(page));
    page.mask = 0;
    page.visible = 0;

    out_line[0] = 0x12345678u; /* overlay=1なら不変のはずの番兵値 */
    x68k_render_gvram_line(gvram, out_line, 1, &page, 1, pal_argb, pal_raw, 0, 0, 1);
    assert(out_line[0] == 0x12345678u);
}

/* 7.1-f: パレット変換の結果が0x0000なら透過する。
 * 16/256色の「パレット出力が0なら透過」と同じ規則が65536色にも適用される
 * （XEiJ DrawingMode.java の GS/GTS/TGS_XHCT が「変換後の16bit値!=0なら
 * グラフィック、0なら背面レイヤ」で分岐している）。MMDSPは輝度最低で
 * グラフィックパレットを全ゼロにするため、この規則が無いと背面のBG面が
 * 黒で塗り潰されて消える。 */
static void test_zero_output_transparent(void) {
    static uint8_t gvram[GVRAM_BYTES];
    gvram_page_t page;
    uint32_t pal_argb[256];
    uint16_t pal_raw[256];
    uint32_t out_line[1];

    memset(gvram, 0, sizeof(gvram));
    memset(pal_argb, 0, sizeof(pal_argb));
    memset(pal_raw, 0, sizeof(pal_raw)); /* MMDSPのトーン0テーブル相当（全ゼロ） */
    memset(&page, 0, sizeof(page));
    page.mask = 0;
    page.visible = 1;

    /* コード自体は非0。変換結果が0になることで透過する。 */
    gvram[0] = 0x7B;
    gvram[1] = 0xDE;

    out_line[0] = 0x12345678u; /* overlay=1なら下地が残る */
    x68k_render_gvram_line(gvram, out_line, 1, &page, 1, pal_argb, pal_raw, 0, 0, 1);
    assert(out_line[0] == 0x12345678u);

    /* overlay=0（最背面）なら従来どおり黒で塗る。 */
    x68k_render_gvram_line(gvram, out_line, 1, &page, 1, pal_argb, pal_raw, 0, 0, 0);
    assert(out_line[0] == 0xFF000000u);

    /* 変換結果が非0なら不透過のまま（下位パレットのIビットだけでも採用する）。
     * code=0x7BDEの下位パレットは L[0xDE] = pal_raw[0xDE]の上位バイト。 */
    pal_raw[0xDE] = 0x0100u; /* L[0xDE] = 0x01 → resolve = 0x0001 */
    out_line[0] = 0x12345678u;
    x68k_render_gvram_line(gvram, out_line, 1, &page, 1, pal_argb, pal_raw, 0, 0, 1);
    assert(out_line[0] == grb555_to_argb(0x0001u)); /* 見た目は黒だが不透過 */
}

int main(void) {
    test_address_mapping();
    test_identity();
    test_brightness_down();
    test_render_line();
    test_invisible_transparent();
    test_zero_output_transparent();
    puts("gfx_palette_64k_test: ALL OK");
    return 0;
}
