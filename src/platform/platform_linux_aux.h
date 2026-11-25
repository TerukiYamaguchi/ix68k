#pragma once

#include "platform/platform_aux_menu.h"
#define SDL_ENABLE_OLD_NAMES 1
#include <SDL3/SDL.h>
#include <stdint.h>

/*
 * Linux(X11/Wayland、SDL3)版のFile/Machine/View/Toolsメニュー実装。
 * Linuxデスクトップにはリポジトリが他OSで使うmacOSのCocoaメニューバー
 * （platform_macos_aux.m）やWindowsのWin32 HMENU（platform_win_aux.c）に
 * 相当するネイティブAPIが存在しない。そのため、ウィンドウ上部に専用の
 * 帯（高さPAL_LINUX_MENU_BAR_H）を確保し、SDLで直接描画する。
 *
 * メニューバー本体（4ボタン）はウィンドウが常時確保する領域（pal_resize/
 * pal_present_frameが他プラットフォームのpal_corner_insetと同じ仕組みで
 * 上部に空ける）に常駐して描く。ドロップダウンは開いている間
 * だけ、合成済みのX68000画面の上に重ねて描く（ネイティブOSのメニューが
 * アプリのクライアント領域に一時的に重なって開くのと同じ挙動）。
 */

/* バー本体の高さ(pt)。platform_sdl3.cのpal_menu_bar_height()と
 * platform_linux_aux.cのレイアウト計算の両方から参照する共通定数。 */
#define PAL_LINUX_MENU_BAR_H 22

#ifdef __cplusplus
extern "C" {
#endif

/* CGROM ANKフォント（メニュー文字の描画に使う）を設定する。ROM/CGROMロード後に
 * 一度呼べばよい（ポインタはアプリ生存中変化しない）。実装: platform_linux_aux.c。 */
void pal_linux_menu_set_cgrom(const uint8_t *cgrom_ank);

/* メニュー宛のイベント（バー領域のクリック/ドロップダウン表示中の全入力）
 * ならここで処理して1を返す。呼び出し側(pal_poll_events)はその場合、
 * 通常のゲスト向け入力処理をスキップすること。メニューと無関係な入力は
 * 0を返し、何もしない。実装: platform_linux_aux.c。 */
int pal_linux_menu_handle_event(const SDL_Event *event);

/* メインのテクスチャ描画後・SDL_RenderPresent前に呼ぶ。バー本体を常に描き、
 * ドロップダウン/サブメニューが開いていればg_rendererへ重ねて描画する。
 * 実装: platform_linux_aux.c。 */
void pal_linux_menu_draw_overlay(void);

#ifdef __cplusplus
}
#endif
