#pragma once
#include <stdint.h>

/*
 * CGROM ANK（半角8x16モノクロ）フォントのビット判定ロジック。
 * SDL等の描画APIに一切依存しない純粋関数のため、実ROMなしでユニット
 * テストできる（tests/aux_font_test.c）。
 *
 * cgrom_ank は bus->iocs_rom + 0x3A000（256エントリ×16バイト）を指す
 * ポインタを呼び出し側が渡す。idx=(ch+0x80)&0xFFが実ROMの_FNTADR計算値
 * （$F3A800 の逆アセンブルで検証済み）と一致する。
 */

/* 文字chのグリフにおいて、(col,row)の画素が点灯していれば1、そうでなければ0。
 * col: 0-7（左から右）、row: 0-15（上から下）。範囲外のcol/rowは0を返す。 */
int pal_aux_ank_glyph_pixel(const uint8_t *cgrom_ank, uint8_t ch, int col, int row);
