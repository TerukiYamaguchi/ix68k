#include "render/renderer.h"
#include <stddef.h>

static uint16_t gvram_page_code_at(const uint8_t *gvram, const gvram_page_t *page,
                                   int stride_1024, int x, int y) {
    unsigned stride = stride_1024 ? 1024u : 512u;
    unsigned gx = ((unsigned)(page->sx + x)) & (stride - 1u);
    unsigned gy = ((unsigned)(page->sy + y)) & (stride - 1u);
    const uint8_t *cell;
    if (stride_1024) {
        /* 実画面1024×1024は「1ページで2Mバイト」を占め、CPUから見たアドレスは
         * $C00000 + (y*1024 + x)*2 のリニア1面（『Inside X68000』3-2-2節）。
         * VRAMバンク0-3はそのリニア面の左上/右上/左下/右下の512×512領域にあたる。
         * 画面の各象限にどのバンクを出すかは、ビデオコントローラR1下位バイトの
         * GP0-GP3（各2bit、標準値$E4で象限とバンクが1対1）が決める（同3-2-6節）。 */
        unsigned quadrant = (gy >= 512u ? 2u : 0u) + (gx >= 512u ? 1u : 0u);
        unsigned bank = ((unsigned)page->page_sel >> (quadrant * 2u)) & 3u;
        unsigned bx = (bank & 1u) * 512u + (gx & 511u);
        unsigned by = (bank >> 1) * 512u + (gy & 511u);
        cell = gvram + (size_t)(by * 1024u + bx) * 2u;
    } else {
        cell = gvram + (size_t)page->base_off + (size_t)(gy * stride + gx) * 2u;
    }
    if (page->mask == 0)
        return ((uint16_t)cell[0] << 8) | cell[1];
    return cell[1] & (uint8_t)page->mask;
}

void x68k_sample_gvram_codes_line(const uint8_t *gvram, uint16_t *out_codes, int w,
                                  const gvram_page_t *pages, int npages, int stride_1024,
                                  int y) {
    /* 呼び出し側はout_codes[0 .. npages*w)だけを読む。残りは未定義。 */
    for (int p = 0; p < npages; p++)
        for (int x = 0; x < w; x++)
            out_codes[p * w + x] = gvram_page_code_at(gvram, &pages[p], stride_1024, x, y);
}

void x68k_render_gvram_line(const uint8_t *gvram, uint32_t *out_line, int w,
                            const gvram_page_t *pages, int npages, const uint32_t *pal_argb,
                            const uint16_t *pal_raw, int stride_1024, int y, int overlay) {
    for (int x = 0; x < w; x++) {
        uint32_t argb = 0xFF000000u; /* 全ページ透明なら黒（overlay時は書かない） */
        int transparent = 1;
        for (int p = 0; p < npages; p++) {
            if (pages[p].mask == 0 && !pages[p].visible) {
                /* 65536色モードは常に単一ページで、R2(GS4-0)非表示時は
                 * 単純に透過扱いとする。 */
                continue;
            }
            uint8_t idx = 0;
            uint16_t direct_grb = 0;
            if (pages[p].visible) {
                if (pages[p].mask == 0) {
                    direct_grb = gvram_page_code_at(gvram, &pages[p], stride_1024, x, y);
                } else {
                    idx = (uint8_t)gvram_page_code_at(gvram, &pages[p], stride_1024, x, y);
                }
            }
            /* R2(GS4-0)非表示時はGVRAMの実データを読まずインデックス0を使う
             * （下地を透過させる「消える」動作ではなく、パレット0番の色で描画する。
             * ただしパレット0番の値が0x0000＝未設定なら他インデックス同様に透過する。
             * 実機検証で判明・訂正）。65536色は上でvisible=false時に
             * continue済みのため、ここに来るのは常にvisible=trueのケース。 */
            if (pages[p].mask == 0) {
                /* 65536色モード。GVRAMの16bit色コードは上位8bit/下位8bitに分けられ、
                 * それぞれ別のパレット（$E82002/$E82000から4バイトおき）で8bitに変換され、
                 * 連結されて最終的なGRB555になる（『Inside X68000』3-6-2、
                 * XEiJ VideoController.vcnPal8G16H/L で確認）。
                 * 透過判定は16/256色と同じ「パレット出力が0x0000なら透過」。65536色では
                 * 変換後の16bit値そのものが出力なので、それが0なら背面レイヤを透かす
                 * （XEiJ DrawingMode.java の GS/GTS 系が同じ形。65536色は常に単一ページ
                 * なので、透過ならこのループはここで終わる）。 */
                uint16_t grb = x68k_gfx_pal64k_resolve(pal_raw, direct_grb);
                if (grb == 0)
                    continue;
                argb = grb555_to_argb(grb);
                transparent = 0;
                break;
            }
            /* 透過判定はインデックスではなくパレットレジスタの生値で行う。
             * レジスタに色が設定されていれば、インデックス0でも通常の不透過色として
             * 描画する（実機/XEiJ準拠。透過になるのはレジスタが0x0000=未設定のときだけ）。 */
            if (pal_raw[idx] != 0) {
                argb = pal_argb[idx];
                transparent = 0;
                break;
            }
        }
        if (!transparent || !overlay)
            out_line[x] = argb;
    }
}

void x68k_render_gvram(const uint8_t *gvram, uint32_t *out, int w, int h,
                       const gvram_page_t *pages, int npages, const uint16_t *palette,
                       int stride_1024, int overlay) {
    uint32_t pal_argb[256];
    for (int i = 0; i < 256; i++)
        pal_argb[i] = grb555_to_argb(palette[i]);
    for (int y = 0; y < h; y++) {
        x68k_render_gvram_line(gvram, out + (size_t)y * w, w, pages, npages, pal_argb, palette,
                               stride_1024, y, overlay);
    }
}
