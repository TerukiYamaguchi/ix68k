#pragma once

#include "platform/platform_aux_menu.h"

/*
 * platform_macos_aux.m（Objective-C、macOSのみビルド）が実装するCocoa連携
 * 関数の宣言。プレーンCの platform_sdl3.c から呼べるようにヘッダを分離する。
 * メニューのコールバック/クエリ関数自体はmacOS固有ではないため
 * platform_aux_menu.h（プラットフォーム非依存）で宣言する。
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 補助ウィンドウのNSWindowに NSWindowCollectionBehaviorFullScreenAuxiliary を
 * 設定する。メインウィンドウがフルスクリーン中でもこの補助ウィンドウを同じ
 * Spaceに表示でき、Space切り替えを起こさない。
 *
 * nswindow_ptr: SDL_GetPointerProperty(SDL_GetWindowProperties(window),
 *               SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL) で得たポインタを
 *               そのまま渡す。NULLなら何もしない。
 */
void pal_macos_set_fullscreen_auxiliary(void *nswindow_ptr);

/*
 * メニューバーに「File」メニュー（無ければ新設）を作り、「Open Disk Selector...」
 * （Cmd+O）以下の項目（ソフトウェアキーボード、Interrupt、Reset、
 * Power Switch、Window Size）を追加する。
 * pal_init() 内、SDL_CreateWindow 成功後（NSApp/
 * mainMenuが存在する時点）に一度だけ呼ぶこと。実装: platform_macos_aux.m。
 */
void pal_macos_install_open_menu_item(void);

/* 開発ビルド（.app外）でDockアイコンを設定する。SDL_CreateWindow成功後に一度呼ぶ。 */
void pal_macos_set_dev_dock_icon(void);

#ifdef __cplusplus
}
#endif
