#include "render/renderer.h"
#include "bus/bus_internal.h" /* SP_PCG_OFF 等のスプライト/BGアドレス定数用 */
#include <stddef.h>

/*
 * PCGエリアの8×8セル cell_no の
 * (cx,cy)（各0-7）位置の4bit色コードを返す。
 * ロングワード(4バイト=1行8ドット)は
 * big-endian、最上位4bit(bit31-28)が左端(cx=0)、
 * 最下位4bit(bit3-0)が右端(cx=7)（『Inside X68000』 図8）。
 */
static inline uint8_t pcg_nibble_8x8(const uint8_t *sp, int cell_no, int cx, int cy) {
    const uint8_t *lw = sp + SP_PCG_OFF + (size_t)cell_no * 32u + (size_t)cy * 4u;
    uint32_t v = ((uint32_t)lw[0] << 24) | ((uint32_t)lw[1] << 16) | ((uint32_t)lw[2] << 8) |
                 (uint32_t)lw[3];
    return (uint8_t)((v >> ((7 - cx) * 4)) & 0xFu);
}

/*
 * 16×16パタン pat_no の (px,py)（各0-15）位置の4bit色コードを返す。
 * 8×8セル4つ(pat_no*4 〜 pat_no*4+3)を 左上/左下/右上/右下
 * の順に組み合わせる （『Inside X68000』 図8）。
 */
static inline uint8_t pcg_nibble_16x16(const uint8_t *sp, int pat_no, int px, int py) {
    int sub = (px < 8 ? 0 : 2) + (py < 8 ? 0 : 1); /* TL=0/BL=1/TR=2/BR=3 */
    return pcg_nibble_8x8(sp, pat_no * 4 + sub, px & 7, py & 7);
}

/* スプライトスクロールレジスタ:
 * $EB0000起点、1組8バイト(4ワード)、128組 */
#define SPR_W(sp, n, off) (((uint16_t)(sp)[(n) * 8 + (off)] << 8) | (sp)[(n) * 8 + (off) + 1])

/*
 * スキャンラインyに掛かる表示ONスプライトを番号昇順(0→127、ハード走査順)に数え、
 * 先頭32個だけ visible[n]=1
 * とする（同一水平線32個制限、33個目以降は非表示）。
 */
static void compute_visible_sprites(const uint8_t *sp_vram, int y, int visible[128]) {
    int count = 0;
    for (int n = 0; n < 128; n++) {
        visible[n] = 0;
        uint16_t w3 = SPR_W(sp_vram, n, 6);
        if ((w3 & 0x3u) == 0)
            continue; /* PRW=00: 非表示 */
        int ypos = SPR_W(sp_vram, n, 2) & 0x3FF;
        int y0 = ypos - 16;
        if (y < y0 || y >= y0 + 16)
            continue;
        if (count < 32) {
            visible[n] = 1;
            count++;
        }
    }
}

/*
 * PRW値が prw_want かつ visible[n]
 * のスプライトだけを番号降順(127→0)でスタンプする。
 * 集合決定（昇順）と描画順（降順）が異なる点に注意:
 * 低番号ほど後書き=最前面になる （実機標準挙動）。y
 * はスキャンライン座標。
 */
static void stamp_sprites(const uint8_t *sp_vram, int *line, int y, int w, int prw_want,
                          const int visible[128]) {
    for (int n = 127; n >= 0; n--) {
        if (!visible[n])
            continue;
        uint16_t w3 = SPR_W(sp_vram, n, 6);
        if ((int)(w3 & 0x3u) != prw_want)
            continue;
        int ypos = SPR_W(sp_vram, n, 2) & 0x3FF;
        int y0 = ypos - 16;
        if (y < y0 || y >= y0 + 16)
            continue;
        int xpos = SPR_W(sp_vram, n, 0) & 0x3FF;
        int x0 = xpos - 16;
        uint16_t w2 = SPR_W(sp_vram, n, 4);
        int vr = (w2 >> 15) & 1;
        int hr = (w2 >> 14) & 1;
        int color = (w2 >> 8) & 0xF;
        int spat = w2 & 0xFF;
        int py = y - y0;
        int ppy = vr ? (15 - py) : py;
        for (int col = 0; col < 16; col++) {
            int rx = x0 + col;
            if (rx < 0 || rx >= w)
                continue;
            int ppx = hr ? (15 - col) : col;
            uint8_t nib = pcg_nibble_16x16(sp_vram, spat, ppx, ppy);
            if (nib)
                line[rx] = (color << 4) | nib;
        }
    }
}

/*
 * BG0/BG1の1スキャンライン分を line[]
 * にスタンプする。実画面座標を球面スクロール （real座標 =
 * (画面座標+スクロール量) mod real_wh）でBG実画面（64×64パタン）に
 * マッピングし、BGデータエリアのエントリ（VR=b15,HR=b14,COLOR=b11-8,PCG#=b7-0）を
 * 引いてPCGドットを取り出す（『Inside X68000』 図9）。
 */
static void stamp_bg_line(const uint8_t *sp_vram, int *line, int y, int w,
                          const sprite_scene_t *sc, int base_off, int sx, int sy) {
    const uint8_t *bgdata = sp_vram + base_off;
    int pat_size = sc->pat_size;
    int real_wh = sc->real_wh;
    /* real_wh(512/1024)・pat_size(8/16)は常に2のべき乗だが実行時変数のため、
     * コンパイラは%/を自動的にAND/シフトへ変換できない。手動でAND/シフトへ
     * 置換して整数除算命令を避ける（Pi Zero(ARMv6)対応を
     * 見据えた最適化）。 */
    unsigned real_mask = (unsigned)real_wh - 1u;
    int pat_mask = pat_size - 1;
    int pat_shift = (pat_size == 16) ? 4 : 3;
    unsigned real_y = ((unsigned)(y + sy)) & real_mask;
    int row = (int)real_y >> pat_shift;
    int ly = (int)real_y & pat_mask;
    for (int x = 0; x < w; x++) {
        unsigned real_x = ((unsigned)(x + sx)) & real_mask;
        int col = (int)real_x >> pat_shift;
        int lx = (int)real_x & pat_mask;
        int off = row * 0x80 + col * 2;
        uint16_t entry = ((uint16_t)bgdata[off] << 8) | bgdata[off + 1];
        int vr = (entry >> 15) & 1;
        int hr = (entry >> 14) & 1;
        int color = (entry >> 8) & 0xF;
        int pcgnum = entry & 0xFF;
        int ppx = hr ? (pat_size - 1 - lx) : lx;
        int ppy = vr ? (pat_size - 1 - ly) : ly;
        uint8_t nib = (pat_size == 16) ? pcg_nibble_16x16(sp_vram, pcgnum, ppx, ppy)
                                       : pcg_nibble_8x8(sp_vram, pcgnum, ppx, ppy);
        if (nib)
            line[x] = (color << 4) | nib;
    }
}

void x68k_sample_sp_codes_line(const uint8_t *sp_vram, int16_t *out_codes, int w,
                               const sprite_scene_t *sc, int y) {
    if (!sc->disp_on) {
        for (int x = 0; x < w; x++)
            out_codes[x] = -1;
        return;
    }
    int line[1024]; /* パレットインデックス(0-255)、-1=透過（未着色） */
    for (int x = 0; x < w; x++)
        line[x] = -1;
    int visible[128];
    compute_visible_sprites(sp_vram, y, visible);
    /* 背面→前面: PRW=01のSP → BG1 → PRW=10のSP → BG0 → PRW=11のSP
     * （『Inside X68000』 3-2-2節）。 */
    stamp_sprites(sp_vram, line, y, w, 1, visible);
    if (sc->bg1_on)
        stamp_bg_line(sp_vram, line, y, w, sc, sc->bg1_base_off, sc->bg1_sx, sc->bg1_sy);
    stamp_sprites(sp_vram, line, y, w, 2, visible);
    if (sc->bg0_on)
        stamp_bg_line(sp_vram, line, y, w, sc, sc->bg0_base_off, sc->bg0_sx, sc->bg0_sy);
    stamp_sprites(sp_vram, line, y, w, 3, visible);

    for (int x = 0; x < w; x++)
        out_codes[x] = (int16_t)line[x];
}

void x68k_render_sp_line(const uint8_t *sp_vram, uint32_t *out_line, int w,
                         const sprite_scene_t *sc, const uint32_t *pal_argb, int y,
                         int overlay) {
    int16_t codes[1024];
    x68k_sample_sp_codes_line(sp_vram, codes, w, sc, y);
    for (int x = 0; x < w; x++) {
        if (codes[x] >= 0)
            out_line[x] = pal_argb[codes[x]];
        else if (!overlay)
            out_line[x] = 0xFF000000u;
    }
}

void x68k_render_sp(const uint8_t *sp_vram, uint32_t *out, int w, int h,
                    const sprite_scene_t *sc, const uint16_t *palette, int overlay) {
    if (!sc->disp_on)
        return;
    uint32_t pal_argb[256];
    for (int i = 0; i < 256; i++)
        pal_argb[i] = grb555_to_argb(palette[i]);
    for (int y = 0; y < h; y++) {
        x68k_render_sp_line(sp_vram, out + (size_t)y * w, w, sc, pal_argb, y, overlay);
    }
}
