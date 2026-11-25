#include "render/renderer.h"
#include "render/video_effects.h"
#include <stddef.h>

#define R2_MAX_WIDTH 1024

static int r2_layer_position(const int order[3], int layer) {
    for (int i = 0; i < 3; i++)
        if (order[i] == layer)
            return i;
    return 0;
}

typedef struct {
    int found, page, in_region;
    uint16_t code, color;
} r2_base_t;

static r2_base_t r2_find_base(const x68k_r2_state_t *r2, const gvram_page_t *pages, int npages,
                              const uint16_t *codes, int w, int x, const uint16_t *palette) {
    r2_base_t base = {0, -1, 0, 0, 0};
    for (int p = 0; p < npages; p++) {
        uint16_t code = codes[p * w + x];
        if (!pages[p].visible)
            continue;
        int is_64k = pages[p].mask == 0;
        /* 16/256色: コード0のページはベースにならない（XEiJ F2_XHCT/E2_XHCT と同じ順序）。
         * 65536色は1ページのみなのでこのガードを通さないが、
         * パレット出力が$0000のドットは透過して背面レイヤを出す（XEiJ TGS_XHCT の
         * 「領域外かつ色0ならスプライト」分岐。MMDSPの輝度最低＝パレット全ゼロで
         * BG面が消えていた不具合の修正）。
         * 奇数コード（B/P=1）またはパレット出力のLSB(B/P=0)が領域内なら、偶数側パレットが
         * $0000でも黒の不透明画素になる。 */
        int in_region = x68k_r2_in_region(r2, is_64k, palette, code);
        uint16_t color = x68k_r2_base_color(r2, is_64k, palette, code);
        int candidate = is_64k ? (in_region || color != 0)
                               : (code != 0 && (in_region || palette[code] != 0));
        if (candidate) {
            base.found = 1;
            base.page = p;
            base.code = code;
            base.in_region = in_region;
            base.color = color;
            return base;
        }
    }
    return base;
}

/* 特殊プライオリティでベースを取り除いた後に見える、残りのGRページの通常色。 */
static int r2_rest_gfx_color(const gvram_page_t *pages, int npages, const uint16_t *codes,
                             int w, int x, const uint16_t *palette, int base_page,
                             uint16_t *out) {
    for (int p = base_page + 1; p < npages; p++) {
        uint16_t code = codes[p * w + x];
        if (!pages[p].visible)
            continue;
        if (pages[p].mask == 0 || palette[code] != 0) {
            *out = pages[p].mask == 0 ? x68k_gfx_pal64k_resolve(palette, code) : palette[code];
            return 1;
        }
    }
    return 0;
}

static int r2_layer_color(int layer, int include, int text_visible, int sprite_visible,
                          const sprite_scene_t *sc, const uint8_t *text_codes,
                          const int16_t *sp_codes, const uint16_t *text_palette, int x,
                          uint16_t *out) {
    if (!include)
        return 0;
    if (layer == 1) {
        uint8_t code = text_codes[x];
        if (!text_visible || text_palette[code] == 0)
            return 0;
        *out = text_palette[code];
        return 1;
    }
    if (layer == 2 && sprite_visible && sc->disp_on && sp_codes[x] >= 0) {
        *out = text_palette[(uint16_t)sp_codes[x]];
        return 1;
    }
    return 0;
}

/* R2特殊表示用の1行合成。通常経路には入らず、生コードをGRB555で解決してからARGB化する。 */
static void compose_line_r2(x68k_bus_t *bus, uint32_t *out_line, int w, int y, int mask,
                            const x68k_r2_state_t *r2) {
    gvram_page_t pages[4];
    int npages = x68k_bus_get_gvram_pages(bus, pages);
    sprite_scene_t sc;
    x68k_bus_get_sprite_scene(bus, &sc);
    int order[3];
    x68k_bus_get_screen_order(bus, order);
    const uint16_t *gfx_palette = x68k_bus_get_gfx_palette_raw(bus);
    const uint16_t *text_palette = x68k_bus_get_text_palette_raw(bus);
    int text_visible = x68k_bus_get_text_visible(bus);
    int sprite_visible = x68k_bus_get_sprite_visible(bus);
    int scroll_x = x68k_bus_get_tvram_scroll_x(bus);
    int scroll_y = x68k_bus_get_tvram_scroll_y(bus);
    uint16_t gfx_codes[4 * R2_MAX_WIDTH];
    uint8_t text_codes[R2_MAX_WIDTH];
    int16_t sp_codes[R2_MAX_WIDTH];
    int include_gr = (mask & X68K_LAYER_GVRAM) != 0;
    int include_text = (mask & X68K_LAYER_TVRAM) != 0;
    int include_sp = (mask & X68K_LAYER_SPRITE) != 0;

    int gr_pos = r2_layer_position(order, 0);
    int active_gr = include_gr;
    int active_text = include_text && text_visible;
    int active_sp = include_sp && sprite_visible && sc.disp_on;
    int exists_behind = 0;
    for (int i = 0; i < gr_pos; i++) {
        if (order[i] == 1 && active_text)
            exists_behind = 1;
        if (order[i] == 2 && active_sp)
            exists_behind = 1;
    }
    if (active_gr)
        x68k_sample_gvram_codes_line(x68k_bus_get_gvram(bus), gfx_codes, w, pages, npages,
                                     x68k_bus_is_gvram_1024dot(bus), y);
    if (active_text)
        x68k_sample_tvram_codes_line(x68k_bus_get_tvram(bus), text_codes, w, scroll_x, scroll_y,
                                     y);
    if (active_sp)
        x68k_sample_sp_codes_line(x68k_bus_get_sp_vram(bus), sp_codes, w, &sc, y);
    for (int x = 0; x < w; x++) {
        r2_base_t base = active_gr
                             ? r2_find_base(r2, pages, npages, gfx_codes, w, x, gfx_palette)
                             : (r2_base_t){0, -1, 0, 0, 0};
        int base_page = base.page;
        uint16_t base_color = base.color;
        int have_base = base.found;
        int base_in_region = have_base && base.in_region;
        int promote_base = base_in_region && r2->effect == X68K_R2_EFFECT_SPECIAL_PRIORITY;
        uint16_t rest_gfx_color = 0;
        int have_rest_gfx =
            promote_base && r2_rest_gfx_color(pages, npages, gfx_codes, w, x, gfx_palette,
                                              base_page, &rest_gfx_color);

        if (base_in_region) {
            if (r2->effect == X68K_R2_EFFECT_HALF_PALETTE0) {
                base_color = x68k_mix_grb555(base_color, text_palette[0]);
            } else {
                if (r2->blend_second && base_page + 1 < npages) {
                    uint16_t second_code = gfx_codes[(base_page + 1) * w + x];
                    uint16_t second_color =
                        pages[base_page + 1].mask == 0
                            ? x68k_gfx_pal64k_resolve(gfx_palette, second_code)
                            : gfx_palette[second_code | 1u];
                    base_color = x68k_mix_grb555(base_color, second_color);
                }
                if (r2->blend_text && (include_text || include_sp)) {
                    /* GRより背面のレイヤが1つでも合成対象として存在する（mask含む、
                     * かつTON/SON/disp_onで有効）ときだけtext_palette[0]を既定値にする。
                     * 「存在するが透明」と「そもそも背面に無い」を区別する（
                     * §10.9。GRが最背面ならgr_pos==0でこのループ自体が回らずリテラル0のまま）。
                     */
                    uint16_t behind = exists_behind ? text_palette[0] : 0;
                    for (int i = 0; i < gr_pos; i++) {
                        uint16_t layer_color;
                        if (r2_layer_color(order[i], order[i] == 1 ? include_text : include_sp,
                                           text_visible, sprite_visible, &sc, text_codes,
                                           sp_codes, text_palette, x, &layer_color))
                            behind = layer_color;
                    }
                    base_color = x68k_mix_grb555(base_color, behind);
                }
            }
        }

        uint16_t result = 0;
        for (int i = 0; i < 3; i++) {
            uint16_t layer_color;
            switch (order[i]) {
            case 0:
                if (promote_base) {
                    if (have_rest_gfx)
                        result = rest_gfx_color;
                } else if (have_base) {
                    result = base_color;
                }
                break;
            case 1:
            case 2:
                if (r2_layer_color(order[i], order[i] == 1 ? include_text : include_sp,
                                   text_visible, sprite_visible, &sc, text_codes, sp_codes,
                                   text_palette, x, &layer_color))
                    result = layer_color;
                break;
            }
        }
        if (promote_base)
            result = base_color;
        out_line[x] = grb555_to_argb(result);
    }

    unsigned contrast = x68k_bus_get_contrast_scaled(bus);
    if (contrast < 45u) {
        const uint8_t *lut = x68k_bus_get_contrast_lut(bus);
        for (int x = 0; x < w; x++) {
            uint32_t px = out_line[x];
            out_line[x] = (px & 0xFF000000u) | ((uint32_t)lut[(px >> 16) & 0xFFu] << 16) |
                          ((uint32_t)lut[(px >> 8) & 0xFFu] << 8) | (uint32_t)lut[px & 0xFFu];
        }
    }
}

/*
 * GR/TX/SP+BGの3レイヤを、x68k_bus_get_screen_order() が返す背面→前面の順で
 * 表示行 y の 1 行だけ合成する。
 * 最背面のみ overlay=0（透過部を黒で塗る）、以降は overlay=1（透過部は書かない）。
 * SPレイヤは sc.disp_on（$EB0808 bit9 DISP/CPU）またはSON（R2 bit6）が0のときは
 * 何も描かず下地を素通しする。TXレイヤはTON（R2 bit5）が0のとき
 * 同様に素通しする。
 *
 * 呼ばれるたびに pages/sc/order/スクロール値を取り直す。パレットは通常V-DISP開始時の
 * snapshotを参照する（-raster-pal live時とラスタ未初期化時はlive）。これが
 * 「そのラスタを描く瞬間の値で合成する」という本設計の核であり、
 * スナップショット構造体は一切持たない（テキストYスクロール等の縦方向値は
 * x68k_bus_get_tvram_scroll_y()/x68k_bus_get_gvram_pages()内部でCRTCラスターが
 * 保持するラッチ値を返す）。
 */
void x68k_compose_line(x68k_bus_t *bus, uint32_t *fb, int w, int h, int y) {
    (void)h;
    x68k_r2_state_t r2;
    x68k_bus_get_r2_state(bus, &r2);
    if (w <= R2_MAX_WIDTH &&
        (r2.effect == X68K_R2_EFFECT_HALF_PALETTE0 || r2.effect == X68K_R2_EFFECT_HALF ||
         r2.effect == X68K_R2_EFFECT_SPECIAL_PRIORITY)) {
        compose_line_r2(bus, fb + (size_t)y * w, w, y,
                        X68K_LAYER_GVRAM | X68K_LAYER_TVRAM | X68K_LAYER_SPRITE, &r2);
        return;
    }
    gvram_page_t pages[4];
    int npages = x68k_bus_get_gvram_pages(bus, pages);
    sprite_scene_t sc;
    x68k_bus_get_sprite_scene(bus, &sc);
    int order[3];
    x68k_bus_get_screen_order(bus, order);
    const uint32_t *gfx_pal_argb = x68k_bus_get_gfx_palette_argb(bus);
    const uint16_t *gfx_pal_raw = x68k_bus_get_gfx_palette_raw(bus);
    const uint32_t *text_pal_argb = x68k_bus_get_text_palette_argb(bus);
    const uint16_t *text_pal_raw = x68k_bus_get_text_palette_raw(bus);
    int scroll_x = x68k_bus_get_tvram_scroll_x(bus);
    int scroll_y = x68k_bus_get_tvram_scroll_y(bus);
    uint32_t *out_line = fb + (size_t)y * w;

    for (int i = 0; i < 3; i++) {
        int ov = (i == 0) ? 0 : 1;
        switch (order[i]) {
        case 0:
            x68k_render_gvram_line(x68k_bus_get_gvram(bus), out_line, w, pages, npages,
                                   gfx_pal_argb, gfx_pal_raw, x68k_bus_is_gvram_1024dot(bus), y,
                                   ov);
            break;
        case 1:
            if (x68k_bus_get_text_visible(bus))
                x68k_render_tvram_line(x68k_bus_get_tvram(bus), out_line, w, text_pal_argb,
                                       text_pal_raw, scroll_x, scroll_y, y, ov);
            break;
        case 2:
            if (sc.disp_on && x68k_bus_get_sprite_visible(bus))
                x68k_render_sp_line(x68k_bus_get_sp_vram(bus), out_line, w, &sc, text_pal_argb,
                                    y, ov);
            break;
        }
    }

    /* システムポート#1 ($E8E001) コントラストを合成後の行へ一括適用する。
     * 出力段の減衰としてモデル化するため、各レンダラは無改変で合成後の
     * ARGB行にLUTを掛ける（docs/ix68k-internals/design/video_controller_design.md §6）。 */
    unsigned contrast = x68k_bus_get_contrast_scaled(bus);
    if (contrast < 45u) {
        const uint8_t *lut = x68k_bus_get_contrast_lut(bus);
        for (int x = 0; x < w; x++) {
            uint32_t px = out_line[x];
            out_line[x] = (px & 0xFF000000u) | ((uint32_t)lut[(px >> 16) & 0xFFu] << 16) |
                          ((uint32_t)lut[(px >> 8) & 0xFFu] << 8) | (uint32_t)lut[px & 0xFFu];
        }
    }
}

/*
 * フレーム全体を一括合成する（従来互換・ラスターモデル無効時のフォールバック）。
 * 中身は x68k_compose_line() を h 回呼ぶだけ。
 */
void x68k_compose_frame(x68k_bus_t *bus, uint32_t *fb, int w, int h) {
    for (int y = 0; y < h; y++)
        x68k_compose_line(bus, fb, w, h, y);
}

/*
 * x68k_compose_line() のレイヤー選択版。mask で指定したレイヤーだけを
 * order[] の順で重ね書きの対象に絞り込む。TON/SON/disp_on/GS4-0といった各レイヤー
 * 自身のON/OFF状態は x68k_compose_line() と同じくそのまま反映する（非表示中の
 * レイヤーを指定すれば素直に黒が返る）。「表示状態と無関係に生データを見たい」
 * 場合はCPU一時停止＋read_memoryで代替する。
 * 合成順に最初に実際に描画したレイヤーだけoverlay=0（下地を黒で初期化）、以降は
 * overlay=1。全レイヤーが非表示で1つも描画されなければ黒で塗る。
 */
void x68k_compose_line_layers(x68k_bus_t *bus, uint32_t *fb, int w, int h, int y, int mask) {
    (void)h;
    x68k_r2_state_t r2;
    x68k_bus_get_r2_state(bus, &r2);
    if (w <= R2_MAX_WIDTH &&
        (r2.effect == X68K_R2_EFFECT_HALF_PALETTE0 || r2.effect == X68K_R2_EFFECT_HALF ||
         r2.effect == X68K_R2_EFFECT_SPECIAL_PRIORITY)) {
        compose_line_r2(bus, fb + (size_t)y * w, w, y, mask, &r2);
        return;
    }
    gvram_page_t pages[4];
    int npages = x68k_bus_get_gvram_pages(bus, pages);
    sprite_scene_t sc;
    x68k_bus_get_sprite_scene(bus, &sc);
    int order[3];
    x68k_bus_get_screen_order(bus, order);
    const uint32_t *gfx_pal_argb = x68k_bus_get_gfx_palette_argb(bus);
    const uint16_t *gfx_pal_raw = x68k_bus_get_gfx_palette_raw(bus);
    const uint32_t *text_pal_argb = x68k_bus_get_text_palette_argb(bus);
    const uint16_t *text_pal_raw = x68k_bus_get_text_palette_raw(bus);
    int scroll_x = x68k_bus_get_tvram_scroll_x(bus);
    int scroll_y = x68k_bus_get_tvram_scroll_y(bus);
    uint32_t *out_line = fb + (size_t)y * w;

    int drawn = 0;
    for (int i = 0; i < 3; i++) {
        int layer_bit = 1 << order[i];
        if (!(mask & layer_bit))
            continue;
        int ov = drawn ? 1 : 0;
        switch (order[i]) {
        case 0:
            x68k_render_gvram_line(x68k_bus_get_gvram(bus), out_line, w, pages, npages,
                                   gfx_pal_argb, gfx_pal_raw, x68k_bus_is_gvram_1024dot(bus), y,
                                   ov);
            drawn = 1;
            break;
        case 1:
            if (x68k_bus_get_text_visible(bus)) {
                x68k_render_tvram_line(x68k_bus_get_tvram(bus), out_line, w, text_pal_argb,
                                       text_pal_raw, scroll_x, scroll_y, y, ov);
                drawn = 1;
            }
            break;
        case 2:
            if (sc.disp_on && x68k_bus_get_sprite_visible(bus)) {
                x68k_render_sp_line(x68k_bus_get_sp_vram(bus), out_line, w, &sc, text_pal_argb,
                                    y, ov);
                drawn = 1;
            }
            break;
        }
    }
    if (!drawn) {
        for (int x = 0; x < w; x++)
            out_line[x] = 0xFF000000u;
    }

    /* x68k_compose_line() と同じくコントラストを合成後に一括適用する
     * （MCP screenshot(layers=) が実画面と同じ明るさになるよう揃える）。 */
    unsigned contrast = x68k_bus_get_contrast_scaled(bus);
    if (contrast < 45u) {
        const uint8_t *lut = x68k_bus_get_contrast_lut(bus);
        for (int x = 0; x < w; x++) {
            uint32_t px = out_line[x];
            out_line[x] = (px & 0xFF000000u) | ((uint32_t)lut[(px >> 16) & 0xFFu] << 16) |
                          ((uint32_t)lut[(px >> 8) & 0xFFu] << 8) | (uint32_t)lut[px & 0xFFu];
        }
    }
}
