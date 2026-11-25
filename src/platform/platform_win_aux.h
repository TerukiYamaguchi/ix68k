#pragma once

#include "platform/platform_aux_menu.h"
#define SDL_ENABLE_OLD_NAMES 1
#include <SDL3/SDL.h>

/*
 * platform_win_aux.c（Windowsのみビルド）が実装するWin32メニューバー連携
 * 関数の宣言。macOSのCocoaメニューバー（platform_macos_aux.m）に相当する
 * File/Machine/View/Toolsメニューを、ネイティブのHMENUで再現する
 * （SDL3自体はクロスプラットフォームAPIのためメニューバーを持たない）。
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * メインウィンドウのHWNDへ「File」「Machine」「View」「Tools」
 * メニューを設置する。全項目をオーナードロー（グレー背景・黒文字）にし、
 * WM_COMMAND/WM_INITMENUPOPUP/WM_MEASUREITEM/WM_DRAWITEMの処理のため
 * ウィンドウプロシージャをサブクラス化する。pal_init() 内、
 * SDL_CreateWindow 成功後に一度だけ呼ぶこと。実装: platform_win_aux.c。
 */
void pal_win_install_menu(SDL_Window *window);

#ifdef __cplusplus
}
#endif
