#pragma once
#include <stdint.h>
#include "bus/x68k_bus.h"

/* R2半透明用のGRB555演算。前景の輝度bitは捨て、背景のbit0を保持する。 */
uint16_t x68k_mix_grb555(uint16_t front, uint16_t back);

/*
 * R2半透明・特殊プライオリティの領域判定（
 * video_controller_design.md §5）。
 * lsb_source==PAGE(B/P=1): ベースページの生コードcodeのbit0で判定。
 * lsb_source==COLOR(B/P=0): パレット出力のLSB(輝度ビットI)で判定。
 *   16/256色はpal_raw[code&~1]のbit0、65536色はcodeを奇数化してから
 *   パレット解決した値のbit0（§10.4、XEiJ準拠）。
 * is_64kはpages[p].mask==0に対応する。
 */
int x68k_r2_in_region(const x68k_r2_state_t *r2, int is_64k, const uint16_t *pal_raw,
                      uint16_t code);

/*
 * R2半透明・特殊プライオリティのベース表示色（§10.5）。
 * lsb_source==PAGE(B/P=1): 偶数化(code&~1)してからパレット解決する。
 * lsb_source==COLOR(B/P=0): 偶数化せずそのままパレット解決する。
 * is_64kなら x68k_gfx_pal64k_resolve() を、そうでなければ pal_raw[idx] を使う。
 */
uint16_t x68k_r2_base_color(const x68k_r2_state_t *r2, int is_64k, const uint16_t *pal_raw,
                            uint16_t code);
