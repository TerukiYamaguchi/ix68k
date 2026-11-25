#pragma once

/*
 * メニュー（macOS Cocoaメニューバー/Windows Win32メニューバー/Linux SDL自前描画）が
 * 共通で呼び出すコールバックとチェックマーク用クエリ関数の宣言。実装は
 * platform_sdl3.c / platform_sdl3_aux_window.c（プラットフォーム非依存）。
 * 各プラットフォーム固有のメニュー構築コードは platform_macos_aux.h/.m・
 * platform_win_aux.h/.c・platform_linux_aux.h/.c が本ヘッダを include して利用する。
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 「File > Open Disk Selector...」選択時にメニュー側から呼ばれるコールバック。
 * F12ホットキーと同じ経路（g_aux_hotkey_req）でディスク選択ウィンドウを
 * 開かせる。実装: platform_sdl3_aux_window.c。
 */
void pal_aux_menu_trigger_disk_selector(void);

/*
 * 「Tools > Open Software Keyboard...」選択時にネイティブメニュー側から
 * 呼ばれるコールバック。Shift+F12ホットキーと同じ経路（g_aux_hotkey_req）で
 * ソフトウェアキーボードウィンドウを開かせる。実装: platform_sdl3_aux_window.c。
 */
void pal_aux_menu_trigger_keyboard(void);

/*
 * 「Help > About ix68k...」選択時にネイティブメニュー側から呼ばれるコールバック。
 * バージョンを表示するダイアログを出す。実装: platform_sdl3_aux_window.c。
 */
void pal_aux_menu_trigger_about(void);

/*
 * 「Machine > Interrupt」選択時にネイティブメニュー側から呼ばれるコールバック。
 * Pauseキーと同じNMI要求（pal_nmi_requested()で消費）をセットする。
 * 実装: platform_sdl3.c。
 */
void pal_aux_menu_trigger_interrupt(void);

/*
 * 「Machine > Power Switch (OFF)」選択時にネイティブメニュー側から呼ばれるコールバック。
 * 前面電源スイッチOFF要求をセットする（pal_power_switch_off_requested()で消費）。
 * 実装: platform_sdl3.c。
 */
void pal_aux_menu_trigger_power_switch_off(void);

/*
 * 「Machine > Reset」選択時にネイティブメニュー側から呼ばれるコールバック。
 * PAL_AUX_HOTKEY_RESETをg_aux_hotkey_reqへセットする。実装: platform_sdl3_aux_window.c。
 */
void pal_aux_menu_trigger_reset(void);

/*
 * 「Machine > Reset (Force FD0 Boot)」選択時にネイティブメニュー側から呼ばれる
 * コールバック。PAL_AUX_HOTKEY_RESET_FD0をg_aux_hotkey_reqへセットする。
 * 実装: platform_sdl3_aux_window.c。実機の「OPT.1を押しながら電源投入/リセット」の
 * 再現（強制的にFD0から起動する）。
 */
void pal_aux_menu_trigger_reset_fd0(void);

/*
 * 「View」メニュー(x1/x1.5/x2)選択時にネイティブメニュー側から
 * 呼ばれるコールバック。対応するPAL_AUX_HOTKEY_ZOOM_*をg_aux_hotkey_reqへ
 * セットする。実装: platform_sdl3_aux_window.c。main.cのメインループが
 * ウィンドウサイズを再計算しpal_resize()で反映する。
 */
void pal_aux_menu_trigger_zoom_1(void);
void pal_aux_menu_trigger_zoom_1_5(void);
void pal_aux_menu_trigger_zoom_2(void);

/*
 * 現在のウィンドウ表示倍率(x10、10=等倍/15=x1.5/20=x2)を返す。
 * 「View」メニューがチェックマーク表示に使う。
 * 実装: platform_sdl3_aux_window.c。
 */
int pal_aux_menu_current_window_zoom_x10(void);

/*
 * 「View > Fullscreen」選択時にネイティブメニュー側から呼ばれる
 * コールバック。F11ホットキーと同じ経路(g_aux_hotkey_req)でフルスクリーンを
 * トグルさせる。実装: platform_sdl3_aux_window.c。
 */
void pal_aux_menu_trigger_fullscreen_toggle(void);

/*
 * 現在フルスクリーン中かどうかを返す。「View > Fullscreen」項目の
 * チェックマーク表示に使う。実装: platform_sdl3.c。
 */
int pal_is_fullscreen(void);

#ifdef __cplusplus
}
#endif
