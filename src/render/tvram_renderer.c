#include "render/renderer.h"
#include <stddef.h>

/*
 * TVRAM レイアウト（1024px 幅仮想スクリーン）
 *   Plane p オフセット: p * 0x20000
 *   Pixel (x, y): バイト = p*0x20000 + y*128 + x/8, bit = 7-(x&7)
 */
#define TVRAM_PLANE_SIZE 0x20000u /* 1プレーンあたり128KB */
#define TVRAM_ROW_BYTES  128u     /* 1024 pixels / 8 bits */

static uint8_t tvram_code_at(const uint8_t *tvram, int scroll_x, int scroll_y, int x, int y) {
    int tvram_y = (y + scroll_y) & 1023;
    int tx = (x + scroll_x) & 1023;
    int off = tvram_y * (int)TVRAM_ROW_BYTES + tx / 8;
    int bit = 7 - (tx & 7);
    return (uint8_t)(((tvram[0 * TVRAM_PLANE_SIZE + off] >> bit) & 1) |
                     (((tvram[1 * TVRAM_PLANE_SIZE + off] >> bit) & 1) << 1) |
                     (((tvram[2 * TVRAM_PLANE_SIZE + off] >> bit) & 1) << 2) |
                     (((tvram[3 * TVRAM_PLANE_SIZE + off] >> bit) & 1) << 3));
}

void x68k_sample_tvram_codes_line(const uint8_t *tvram, uint8_t *out_codes, int w, int scroll_x,
                                  int scroll_y, int y) {
    for (int x = 0; x < w; x++)
        out_codes[x] = tvram_code_at(tvram, scroll_x, scroll_y, x, y);
}

void x68k_render_tvram_line(const uint8_t *tvram, uint32_t *out_line, int w,
                            const uint32_t *pal_argb, const uint16_t *pal_raw, int scroll_x,
                            int scroll_y, int y, int overlay) {
    for (int x = 0; x < w; x++) {
        int c = tvram_code_at(tvram, scroll_x, scroll_y, x, y);
        /* 透過判定はインデックスではなくパレットレジスタの生値で行う。
         * インデックス0でもパレット
         * レジスタに色が設定されていれば通常の不透過色として描画する。透過に
         * なるのはレジスタが0x0000=未設定のときだけ。 */
        if (pal_raw[c] != 0)
            out_line[x] = pal_argb[c];
        else if (!overlay)
            out_line[x] = 0xFF000000u; /* 下地として使う場合は透過部を黒で塗る */
    }
}

void x68k_render_tvram(const uint8_t *tvram, uint32_t *out, int w, int h,
                       const uint16_t *palette, int scroll_y, int overlay) {
    uint32_t pal_argb[16];
    for (int i = 0; i < 16; i++)
        pal_argb[i] = grb555_to_argb(palette[i]);
    for (int y = 0; y < h; y++) {
        /* この API は scroll_x を受け取らないため、水平スクロールは 0 とする。 */
        x68k_render_tvram_line(tvram, out + (size_t)y * w, w, pal_argb, palette, 0, scroll_y, y,
                               overlay);
    }
}
