#pragma once

#include "platform/platform.h"

/* SDL3の旧名を、SDL2版から維持したキー変換表でも使えるようにする。 */
#define SDL_ENABLE_OLD_NAMES 1
#include <SDL3/SDL.h>

/* platform_sdl3.c が所有し、SDL実装ファイルだけで共有する状態。 */
extern SDL_Window *g_window;
extern SDL_Renderer *g_renderer;
extern SDL_AudioStream *g_audio_stream;
extern int g_audio_channels;
extern int g_audio_started;
extern SDL_AudioStream *g_capture_stream;
extern int g_serial_fd;

uint8_t sdl_input_keycode(SDL_Keycode sym);
void sdl_input_send_key(uint8_t kcode, int released);
void sdl_input_key_down(SDL_Keycode sym, SDL_Keymod mod);
void sdl_input_key_up(SDL_Keycode sym);
/* 予約ホットキーなら1を返す。XFはここでゲストへ直接送出し、ウィンドウ系は
 * out_requestへ返す。対応するKEY_UPもsdl_input_hotkey_upで消費する。 */
int sdl_input_hotkey_down(SDL_Keycode sym, SDL_Keymod mod, pal_aux_hotkey_t *out_request);
int sdl_input_hotkey_up(SDL_Keycode sym);
void sdl_input_mouse_enter(void);
void sdl_input_mouse_leave(void);
void sdl_input_mouse_resized(void);
void sdl_input_mouse_motion(int x, int y);
void sdl_input_mouse_button(void);
void sdl_input_set_dimensions(int native_w, int native_h, int display_w, int display_h);
void sdl_input_gamepad_added(SDL_JoystickID device_id);
void sdl_input_gamepad_removed(SDL_JoystickID instance_id);
void sdl_input_debug_joystick_event(const SDL_Event *event);
void sdl_input_init_gamepads(void);
void sdl_input_quit(void);
void joystick_try_open(SDL_JoystickID device_id);
void joystick_close_by_instance(SDL_JoystickID instance_id);
int sdl_aux_window_handle_event(SDL_Window *window, const SDL_Event *event);
void sdl_aux_window_request_hotkey(pal_aux_hotkey_t hotkey);
/* macOS角丸ウィンドウ対策の内側マージン(pt)。フルスクリーン時・非macOSは0。
 * platform_sdl3.cが所有し、pal_resize/pal_present_frame/マウス座標換算で共有する。 */
int pal_corner_inset(void);
/* Linux(SDL自前描画メニュー)のメニューバー高さ(pt)。フルスクリーン時・
 * macOS/Windowsでは0。platform_sdl3.cが所有し、pal_resize/pal_present_frame/
 * マウス座標換算・platform_linux_aux.cのレイアウト計算で共有する。 */
int pal_menu_bar_height(void);
