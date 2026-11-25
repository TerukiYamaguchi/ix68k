/*
 * F12/Shift+F12 → pal_aux_window_take_hotkey() の経路を検証する。
 * test_nmi_pause.c と同じ手法（SDL_PushEvent()合成注入、ヘッドレス実行）で、
 * MCPのkey注入（ゲスト向けキーボードプロトコルへ直接注入するだけで実SDL
 * イベントを経由しない）では検証できない host予約キーの経路を確認する。
 */
#include "platform/platform.h"
#include "platform/platform_sdl3_internal.h"
#define SDL_ENABLE_OLD_NAMES 1
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include "platform/platform_macos_aux.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* setenv() 相当 */
#endif

static void push_f12(SDL_Keymod mod) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = SDLK_F12;
    ev.key.mod = mod;
    ev.key.down = true;
    if (!SDL_PushEvent(&ev)) {
        fprintf(stderr, "SDL_PushEvent failed: %s\n", SDL_GetError());
        exit(1);
    }
}

static void push_key(SDL_Keycode key, SDL_Keymod mod, SDL_EventType type) {
    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.key.type = type;
    ev.key.key = key;
    ev.key.mod = mod;
    ev.key.down = (type == SDL_EVENT_KEY_DOWN);
    assert(SDL_PushEvent(&ev));
}

static uint8_t g_key_bytes[4];
static int g_key_byte_count;

static void capture_key(void *ctx, uint8_t byte) {
    (void)ctx;
    assert(g_key_byte_count < (int)sizeof(g_key_bytes));
    g_key_bytes[g_key_byte_count++] = byte;
}

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);

    if (pal_init("test_aux_hotkey", 320, 240) < 0) {
        fprintf(stderr, "pal_init failed\n");
        return 1;
    }

    /* 押下前: 要求は立っていない */
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_NONE);

    /* 外付けキーボードのF13〜F17とテンキーは、ソフトウェアキーボードを介さず
     * X68000固有キーへ届く。KP Enterは通常Returnとは別の0x4E。 */
    assert(sdl_input_keycode(SDLK_F13) == 0x55);
    assert(sdl_input_keycode(SDLK_F17) == 0x59);
    assert(sdl_input_keycode(SDLK_KP_CLEAR) == 0x3F);
    assert(sdl_input_keycode(SDLK_KP_7) == 0x43);
    assert(sdl_input_keycode(SDLK_KP_ENTER) == 0x4E);
    assert(sdl_input_keycode(SDLK_KP_PERIOD) == 0x51);

    /* 設定ファイル相当のホットキーはF12既定値を置き換え、XFの離上まで
     * ゲストへ正しい物理コードとして送る。 */
    const pal_hotkey_config_t config = {
        .disk_window = "ctrl+f10",
        .software_keyboard = "none",
        .xf = {"ctrl+f1", "none", "none", "none", "none"},
        .kigou = "ctrl+f2",
        .touroku = "none",
        .break_key = "none",
    };
    pal_key_set_hotkey_config(&config);
    pal_key_set_callbacks(capture_key, NULL, NULL);
    push_key(SDLK_F12, SDL_KMOD_NONE, SDL_EVENT_KEY_DOWN);
    pal_poll_events();
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_NONE);
    push_key(SDLK_F10, SDL_KMOD_CTRL, SDL_EVENT_KEY_DOWN);
    pal_poll_events();
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_DISK_WINDOW);
    push_key(SDLK_F1, SDL_KMOD_CTRL, SDL_EVENT_KEY_DOWN);
    push_key(SDLK_F1, SDL_KMOD_CTRL, SDL_EVENT_KEY_UP);
    pal_poll_events();
    assert(g_key_byte_count == 2);
    assert(g_key_bytes[0] == 0x55);
    assert(g_key_bytes[1] == 0xD5);
    push_key(SDLK_F2, SDL_KMOD_CTRL, SDL_EVENT_KEY_DOWN);
    push_key(SDLK_F2, SDL_KMOD_CTRL, SDL_EVENT_KEY_UP);
    pal_poll_events();
    assert(g_key_byte_count == 4);
    assert(g_key_bytes[2] == 0x52);
    assert(g_key_bytes[3] == 0xD2);

    const pal_hotkey_config_t defaults = {
        .disk_window = "f12",
        .software_keyboard = "shift+f12",
        .xf = {"none", "none", "none", "none", "none"},
        .kigou = "none",
        .touroku = "none",
        .break_key = "none",
    };
    pal_key_set_hotkey_config(&defaults);

    /* F12単独 → ディスク選択ウィンドウ要求 */
    push_f12(SDL_KMOD_NONE);
    pal_poll_events();
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_DISK_WINDOW);
    /* 読んだら消費される（1パルスのみ） */
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_NONE);

    /* Shift+F12 → ソフトウェアキーボードウィンドウ要求 */
    push_f12(SDL_KMOD_LSHIFT);
    pal_poll_events();
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_KEYBOARD_WINDOW);
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_NONE);

#ifdef __APPLE__
    /* 「File > Reset」(Cmd+R、ユーザー要望)のCocoaコールバック経路。
     * SDLキーイベントを経由しないため、コールバック関数を直接呼んで
     * g_aux_hotkey_reqが正しくセットされることだけ検証する
     * （メニューのkeyEquivalent自体はAppKitが処理するため対象外）。 */
    pal_aux_menu_trigger_reset();
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_RESET);
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_NONE);

    /* 「File > Reset (Force FD0 Boot)」(Cmd+Option+R）。上と同じ
     * 経路の検証で、Cmd+RとCmd+Option+Rが別のホットキー値として届くことを
     * 確認する。 */
    pal_aux_menu_trigger_reset_fd0();
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_RESET_FD0);
    assert(pal_aux_window_take_hotkey() == PAL_AUX_HOTKEY_NONE);
#endif

    pal_quit();
    printf("test_aux_hotkey: OK\n");
    return 0;
}
