/*
 * SDL PAUSE キー → NMI 要求フラグの経路を検証する。
 *
 * 実キーボードなしで検証するため、SDL_PushEvent() で SDLK_PAUSE の
 * キー押下イベントを合成注入し、platform_sdl3.c の pal_poll_events() が
 * これを処理して pal_nmi_requested() に反映することを確認する。ヘッドレス
 * 実行のため SDL_VIDEODRIVER=dummy を強制する。
 *
 * MCP の INTERRUPT コマンド / send_interrupt ツールは mc68k_set_irq() を
 * 直接呼ぶだけで本経路（SDLイベント→g_nmi_req）を一切通らないため、
 * この経路はそれらのテストでは検証できていなかった。
 */
#include "platform/platform.h"
#define SDL_ENABLE_OLD_NAMES 1
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* setenv() 相当 */
#endif

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);

    if (pal_init("test_nmi_pause", 320, 240) < 0) {
        fprintf(stderr, "pal_init failed\n");
        return 1;
    }

    /* PAUSE 押下前: NMI 要求は立っていない */
    assert(pal_nmi_requested() == 0);

    SDL_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SDL_EVENT_KEY_DOWN;
    ev.key.type = SDL_EVENT_KEY_DOWN;
    ev.key.key = SDLK_PAUSE;
    ev.key.down = true;
    if (!SDL_PushEvent(&ev)) {
        fprintf(stderr, "SDL_PushEvent failed: %s\n", SDL_GetError());
        return 1;
    }

    pal_poll_events();

    /* 1回目: PAUSE で立ったフラグが読める */
    assert(pal_nmi_requested() == 1);
    /* 2回目: read-and-clear なので既に0（1パルスのみ、連続発火しない） */
    assert(pal_nmi_requested() == 0);

    pal_quit();
    printf("test_nmi_pause: OK\n");
    return 0;
}
