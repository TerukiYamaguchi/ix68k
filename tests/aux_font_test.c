/*
 * pal_aux_ank_glyph_pixel()の単体テスト。実ROMを使わず、
 * 既知のビットパターンを持つ合成CGROMバッファでインデックス計算・
 * ビット順（MSBが左端）を検証する。
 */
#include "platform/aux_font.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CGROM_ANK_SIZE (256 * 16)

int main(void) {
    static uint8_t cgrom[CGROM_ANK_SIZE];
    memset(cgrom, 0, sizeof(cgrom));

    /* ch=0x00 → idx=(0+0x80)&0xFF=0x80。0x80番目のエントリの1行目に
     * 0b10110001を置き、col=0,2,3,7が点灯・それ以外が消灯であることを
     * ビット順(MSBが左端=col0)込みで確認する。 */
    int idx_for_ch0 = 0x80;
    cgrom[idx_for_ch0 * 16 + 0] = 0xB1; /* 0b10110001 */
    int expect[8] = {1, 0, 1, 1, 0, 0, 0, 1};
    for (int col = 0; col < 8; col++)
        assert(pal_aux_ank_glyph_pixel(cgrom, 0x00, col, 0) == expect[col]);

    /* 他の行(row=1-15)は0のまま(まだ何も置いていない)ことを確認。 */
    for (int row = 1; row < 16; row++)
        for (int col = 0; col < 8; col++)
            assert(pal_aux_ank_glyph_pixel(cgrom, 0x00, col, row) == 0);

    /* ch=0x41('A') → idx=(0x41+0x80)&0xFF=0xC1。行15に0xFFを置き、
     * 全列点灯を確認(境界の最終行も正しくアクセスできること)。 */
    int idx_for_A = (uint8_t)(0x41 + 0x80);
    cgrom[idx_for_A * 16 + 15] = 0xFF;
    for (int col = 0; col < 8; col++)
        assert(pal_aux_ank_glyph_pixel(cgrom, 0x41, col, 15) == 1);
    /* 'A'グリフの他の行はまだ0（ch=0x00の書き込みと混線していないこと）。 */
    for (int col = 0; col < 8; col++)
        assert(pal_aux_ank_glyph_pixel(cgrom, 0x41, col, 0) == 0);

    /* ch=0xFF → idx=(0xFF+0x80)&0xFF=0x7F（256を跨ぐラップアラウンド）。 */
    int idx_for_wrap = (uint8_t)(0xFFu + 0x80u);
    assert(idx_for_wrap == 0x7F);
    cgrom[idx_for_wrap * 16 + 3] = 0x01; /* col=7のみ点灯 */
    for (int col = 0; col < 7; col++)
        assert(pal_aux_ank_glyph_pixel(cgrom, 0xFF, col, 3) == 0);
    assert(pal_aux_ank_glyph_pixel(cgrom, 0xFF, 7, 3) == 1);

    /* 範囲外のcol/rowは0を返す（境界チェックの回帰確認）。 */
    assert(pal_aux_ank_glyph_pixel(cgrom, 0x41, -1, 0) == 0);
    assert(pal_aux_ank_glyph_pixel(cgrom, 0x41, 8, 0) == 0);
    assert(pal_aux_ank_glyph_pixel(cgrom, 0x41, 0, -1) == 0);
    assert(pal_aux_ank_glyph_pixel(cgrom, 0x41, 0, 16) == 0);

    printf("aux_font_test: all tests passed\n");
    return 0;
}
