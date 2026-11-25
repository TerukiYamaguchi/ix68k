#pragma once
#include <stdint.h>
#include "bus/x68k_bus.h"

/*
 * GRB555 ワードを ARGB8888 に変換する。
 * X68000 GVRAM / テキストパレットフォーマット:
 *   bit 15-11: G (5bit)
 *   bit 10-6 : R (5bit)
 *   bit  5-1 : B (5bit)
 *   bit  0   : 輝度ビット（ここでは無視）
 */
static inline uint32_t grb555_to_argb(uint16_t grb) {
    unsigned g = (grb >> 11) & 0x1Fu;
    unsigned r = (grb >> 6) & 0x1Fu;
    unsigned b = (grb >> 1) & 0x1Fu;
    return 0xFF000000u | ((uint32_t)((r << 3) | (r >> 2)) << 16) |
           ((uint32_t)((g << 3) | (g >> 2)) << 8) | (uint32_t)((b << 3) | (b >> 2));
}

/*
 * 65536色モードのグラフィックパレット（『Inside X68000』3-6-2）。
 * $E82000-$E821FF の512バイトは、16/256色モードでは256エントリ×GRB555だが、
 * 65536色モードでは「8bit×256×2組」として再解釈される。
 *   下位パレット: $E82000 から4バイトおき2バイト。色コード下位8bitを
 *                 R下位2bit+B(5bit)+I(1bit) へ変換する。
 *   上位パレット: $E82002 から4バイトおき2バイト。色コード上位8bitを
 *                 G(5bit)+R上位3bit へ変換する。
 * pal_raw[] は x68k_bus_get_gfx_palette_raw() 由来の16bitエントリ配列なので、
 * 下位パレットは偶数エントリ、上位パレットは奇数エントリの側に載る。
 * k の偶奇（=色コードのそのバイトのLSB）でエントリ内の上位/下位バイトを選ぶ。
 */
static inline unsigned x68k_gfx_pal64k_lo(const uint16_t *pal_raw, unsigned k) {
    uint16_t w = pal_raw[k & ~1u];
    return (k & 1u) ? (unsigned)(w & 0xFFu) : (unsigned)(w >> 8);
}
static inline unsigned x68k_gfx_pal64k_hi(const uint16_t *pal_raw, unsigned k) {
    uint16_t w = pal_raw[k | 1u];
    return (k & 1u) ? (unsigned)(w & 0xFFu) : (unsigned)(w >> 8);
}
/* 65536色モードの色コード code を、上位/下位パレットで変換して GRB555 に組み直す。 */
static inline uint16_t x68k_gfx_pal64k_resolve(const uint16_t *pal_raw, uint16_t code) {
    return (uint16_t)((x68k_gfx_pal64k_hi(pal_raw, (unsigned)(code >> 8)) << 8) |
                      x68k_gfx_pal64k_lo(pal_raw, (unsigned)(code & 0xFFu)));
}

/*
 * R2特殊表示用の、生パレットコード sampler。
 * GVRAMは page-major（out_codes[p * w + x]）にnpagesページ分を格納する。
 * out_codes[npages*w..]は未定義。各samplerは色変換や表示ON/OFFを行わず、VRAMから得た色コード
 * だけを返す。表示ゲートと優先順位はcompose側のR2リゾルバーが判断する。
 */
void x68k_sample_gvram_codes_line(const uint8_t *gvram, uint16_t *out_codes, int w,
                                  const gvram_page_t *pages, int npages, int stride_1024,
                                  int y);
void x68k_sample_tvram_codes_line(const uint8_t *tvram, uint8_t *out_codes, int w, int scroll_x,
                                  int scroll_y, int y);
void x68k_sample_sp_codes_line(const uint8_t *sp_vram, int16_t *out_codes, int w,
                               const sprite_scene_t *sc, int y);

/*
 * グラフィック VRAM を ARGB8888 フレームバッファへ複数ページ合成する。
 *
 * gvram       : 0xC00000 からの生バイト列（2MB 全域）
 * out         : 出力バッファ (w×h ピクセル, ARGB8888)
 * w, h        : 出力解像度（通常 512×512）
 * pages       : x68k_bus_get_gvram_pages() が返すページ記述（優先度順、pages[0] が最前面）
 * npages      : pages の有効要素数（1/2/4）
 * palette     : 256 エントリの GRB555 配列（16色・256色モード時に使用; 65536色では無視）
 * stride_1024 : 非0 なら GVRAM の実ラスタ幅が 1024px（x68k_bus_is_gvram_1024dot 由来）、
 *               0 なら 512px。全ページ共通（1024dot 時は常に単一ページ）。
 * overlay     : 0ならグラフィック画面をテキスト画面より背面（従来のTX>GR固定）として描画し、
 *               全ページ透明なピクセルは黒で塗る。非0なら前面（GR>TX）として
 *               描画し、透明なピクセルは書き込まず下地（テキスト画面）を透過させる。
 *
 * 各ピクセルはページを pages[0] から順に見て、パレット出力!=0x0000 の最初の
 * ページを採用する（パレットインデックス自体ではなく出力値で不透過/透過を
 * 判定する。16/256色はpal_raw[idx]、65536色は x68k_gfx_pal64k_resolve() の
 * 結果がパレット出力）。スクロールは球面式（実ラスタ縦幅で巻き戻す）。
 */
void x68k_render_gvram(const uint8_t *gvram, uint32_t *out, int w, int h,
                       const gvram_page_t *pages, int npages, const uint16_t *palette,
                       int stride_1024, int overlay);

/*
 * x68k_render_gvram() の 1 行版。
 * out_line は出力先の1行分（w要素）を指す。pal_argb は256エントリのARGB8888変換済み
 * パレット（x68k_bus_get_gfx_palette_argb()由来）、pal_raw は同じ256エントリの
 * GRB555生値（x68k_bus_get_gfx_palette_raw()由来）。透過判定はパレットインデックスでは
 * なく pal_raw[idx]==0x0000（パレットレジスタ未設定）で行う（XEiJ
 * DrawingMode.java実装で確認済み: インデックス自体は0でもレジスタに色を設定して
 * いれば不透過の通常色として描画される）。pages[].sx はライブ、
 * pages[].sy はラスターがラッチした値（x68k_bus_get_gvram_pages()参照）。
 * y は表示行番号（0..h-1）。呼び出し側がラスタ同期で毎行 pages/pal_argb/pal_raw を
 * 取り直すことで、ラスタ単位のスクロール演出が成立する。
 * 65536色モード（pages[].mask==0）では pal_argb は使われず、pal_raw が
 * x68k_gfx_pal64k_resolve() 経由で上位/下位パレットとして再解釈される。
 */
void x68k_render_gvram_line(const uint8_t *gvram, uint32_t *out_line, int w,
                            const gvram_page_t *pages, int npages, const uint32_t *pal_argb,
                            const uint16_t *pal_raw, int stride_1024, int y, int overlay);

/*
 * テキスト VRAM (4 ビットプレーン) を ARGB8888 フレームバッファに合成する。
 * 透過判定はインデックスではなくパレットレジスタの生値で行う（GVRAMと同じ
 * のルール。インデックス0でもパレットレジスタに色が設定されて
 * いれば通常の不透過色として描画する。透過になるのはレジスタが0x0000=
 * 未設定のときだけ。動作検証で判明）。
 *
 * tvram   : 0xE00000 からの生バイト列（4 プレーン × 128KB = 512KB）
 * out     : 合成先バッファ（x68k_render_gvram() の出力と同一バッファを想定）
 * w, h    : 出力解像度（通常 512×512）
 * palette : 16 エントリの GRB555 ホストバイトオーダー配列（x68k_bus_get_text_palette() 由来）
 * overlay : 0なら背面（下地）として描画し、透過部を黒で塗る。非0なら前面として描画し、
 *           透過部は書き込まず下地（グラフィック画面）を透過させる。
 */
void x68k_render_tvram(const uint8_t *tvram, uint32_t *out, int w, int h,
                       const uint16_t *palette, int scroll_y, int overlay);

/*
 * x68k_render_tvram() の 1 行版。out_line は出力先の 1 行分（w要素）。
 * pal_argb は16エントリのARGB8888変換済みパレット、pal_raw は同じ16エントリの
 * GRB555生値（透過判定用、x68k_bus_get_text_palette_raw()由来）。scroll_x は CRTC R10
 * （ライブ参照、x68k_bus_get_tvram_scroll_x()由来）、scroll_y は R11（ラスターが
 * ラッチした値、x68k_bus_get_tvram_scroll_y()由来）。y は表示行番号
 * （0..h-1）。
 */
void x68k_render_tvram_line(const uint8_t *tvram, uint32_t *out_line, int w,
                            const uint32_t *pal_argb, const uint16_t *pal_raw, int scroll_x,
                            int scroll_y, int y, int overlay);

/*
 * スプライト+BG画面を ARGB8888 フレームバッファへ合成する。
 *
 * sp_vram : 0xEB0000 からの64KB
 * out     : 合成先バッファ
 * w, h    : 出力解像度
 * sc      : x68k_bus_get_sprite_scene() が返すモード派生値
 * palette : 256エントリのテキスト+スプライト/BG共用パレット（$E82200）
 * overlay : 0なら透過部を黒で塗り、非0なら下地を残す
 *
 * 色コード（PCG下位4bit）が0のドットは透過。不透過時のパレットインデックスは
 * (COLOR<<4)|nibble。低い番号のスプライトほど手前に表示される。
 */
void x68k_render_sp(const uint8_t *sp_vram, uint32_t *out, int w, int h,
                    const sprite_scene_t *sc, const uint16_t *palette, int overlay);

/*
 * x68k_render_sp() の 1 行版。out_line は出力先の 1 行分（w要素）。
 * pal_argb は256エントリのARGB8888変換済みパレット。sc は毎行
 * x68k_bus_get_sprite_scene() で取り直したもの（スプライトYPOS/BGスクロールとも
 * ライブ参照）。y は表示行番号（0..h-1）。
 */
void x68k_render_sp_line(const uint8_t *sp_vram, uint32_t *out_line, int w,
                         const sprite_scene_t *sc, const uint32_t *pal_argb, int y,
                         int overlay);

/*
 * GR/TX/SP+BG の 3 レイヤ合成。
 * x68k_compose_line() は表示行yの1行だけを、呼ばれた瞬間のbus状態で合成する。
 * x68k_compose_frame() はそれを h 回呼ぶだけの互換フォールバック（ラスターモデル無効時用）。
 */
void x68k_compose_line(x68k_bus_t *bus, uint32_t *fb, int w, int h, int y);
void x68k_compose_frame(x68k_bus_t *bus, uint32_t *fb, int w, int h);

/* mask に渡すレイヤービット（x68k_compose_line_layers() 用）。
 * x68k_bus_get_screen_order() が返すレイヤーID（0=GR/1=TX/2=SP）と1:1対応。
 * tools-host/mcp/ix68k_mcp_server.py の LAYER_BITS と値を同期させること。 */
#define X68K_LAYER_GVRAM  (1 << 0)
#define X68K_LAYER_TVRAM  (1 << 1)
#define X68K_LAYER_SPRITE (1 << 2)

/*
 * x68k_compose_line() のレイヤー選択版（MCP screenshot(layers=...)専用）。
 * mask で指定したレイヤーだけを合成対象に絞り込む。TON/SON/disp_on/GS4-0 といった
 * 各レイヤー自身のON/OFF状態は x68k_compose_line() と同じくそのまま反映するため、
 * 指定したレイヤーが現在非表示なら黒が返る（表示状態と無関係の生データが欲しい
 * 場合はCPU一時停止＋read_memoryで代替する）。mask内の全レイヤーが非表示で
 * 1つも描画されなければ黒で塗る。
 */
void x68k_compose_line_layers(x68k_bus_t *bus, uint32_t *fb, int w, int h, int y, int mask);
