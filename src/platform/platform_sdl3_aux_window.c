#include "platform/platform_sdl3_internal.h"
#include "platform/aux_font.h"
#include "platform/platform_aux_menu.h"
#include "x68k_version.h"
#ifdef __APPLE__
#include "platform/platform_macos_aux.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pal_aux_hotkey_t g_aux_hotkey_req = PAL_AUX_HOTKEY_NONE;
/* メニュー「View」(x1/x1.5/x2)チェックマーク用(macOS/Windows/Linuxのメニューが参照)、10=等倍 */
static int g_current_window_zoom_x10 = 10;

void pal_aux_set_current_window_zoom_x10(int zoom_x10) {
    g_current_window_zoom_x10 = zoom_x10;
}

/* ホスト側補助ウィンドウ。同時に開けるのはキーボード・
 * ディスク選択の高々2枠程度の想定だが、余裕を見て4枠。 */
#define PAL_AUX_WINDOW_MAX 4
struct pal_aux_window {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int close_requested;
    int click_pending; /* 離上(クリック確定)時に1、take_click()で消費 */
    int click_x, click_y;
    int mouse_down; /* 現在左ボタンが押し下げられているか（状態値、消費しない） */
    int mouse_x, mouse_y;
    int key_forward_enabled;     /* pal_aux_window_set_key_forward()参照 */
    uint32_t held_kcode_bits[4]; /* kcode(0x00-0x7F)の押下状態ビットマップ */
};
static pal_aux_window_t *g_aux_windows[PAL_AUX_WINDOW_MAX];

void sdl_aux_window_request_hotkey(pal_aux_hotkey_t hotkey) {
    g_aux_hotkey_req = hotkey;
}
/* g_aux_windows[]からSDL_Window*に対応するpal_aux_window_tを探す。 */
static pal_aux_window_t *aux_window_find(SDL_Window *w) {
    for (int i = 0; i < PAL_AUX_WINDOW_MAX; i++)
        if (g_aux_windows[i] && g_aux_windows[i]->window == w)
            return g_aux_windows[i];
    return NULL;
}

/* 補助ウィンドウ宛のイベントを処理する。クリック/クローズ要求を
 * 「次にtake_*()が呼ばれるまで」保持するだけ（描画等はここでは行わない）。 */
static void aux_window_handle_event(pal_aux_window_t *win, const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        win->close_requested = 1;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            win->mouse_down = 1;
            win->mouse_x = (int)e->button.x;
            win->mouse_y = (int)e->button.y;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT && win->mouse_down) {
            win->mouse_down = 0;
            win->click_pending = 1;
            win->click_x = (int)e->button.x;
            win->click_y = (int)e->button.y;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (win->mouse_down) {
            win->mouse_x = (int)e->motion.x;
            win->mouse_y = (int)e->motion.y;
        }
        break;
    case SDL_EVENT_KEY_DOWN: {
        if (!win->key_forward_enabled)
            break;
        uint8_t kcode = sdl_input_keycode(e->key.key);
        if (kcode == 0)
            break;
        win->held_kcode_bits[kcode >> 5] |= (1u << (kcode & 31));
        sdl_input_send_key(kcode, 0); /* bit7=0: 押下 */
        break;
    }
    case SDL_EVENT_KEY_UP: {
        if (!win->key_forward_enabled)
            break;
        uint8_t kcode = sdl_input_keycode(e->key.key);
        if (kcode == 0)
            break;
        win->held_kcode_bits[kcode >> 5] &= ~(1u << (kcode & 31));
        sdl_input_send_key(kcode, 1); /* bit7=1: 離上 */
        break;
    }
    default:
        break;
    }
}

int sdl_aux_window_handle_event(SDL_Window *window, const SDL_Event *event) {
    pal_aux_window_t *aux = aux_window_find(window);
    if (!aux)
        return 0;
    aux_window_handle_event(aux, event);
    return 1;
}
/* ---- ホスト側補助ウィンドウ（共通基盤） ---- */

pal_aux_window_t *pal_aux_window_create(const char *title, int w, int h) {
    int slot = -1;
    for (int i = 0; i < PAL_AUX_WINDOW_MAX; i++) {
        if (!g_aux_windows[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pal_log("pal_aux_window_create: 補助ウィンドウの上限(%d)に達しました\n",
                PAL_AUX_WINDOW_MAX);
        return NULL;
    }

    pal_aux_window_t *win = calloc(1, sizeof(*win));
    if (!win)
        return NULL;

    win->window = SDL_CreateWindow(title, w, h, 0);
    if (!win->window) {
        pal_log("pal_aux_window_create: SDL_CreateWindow失敗: %s\n", SDL_GetError());
        free(win);
        return NULL;
    }
    win->renderer = SDL_CreateRenderer(win->window, NULL);
    if (!win->renderer) {
        pal_log("pal_aux_window_create: SDL_CreateRenderer失敗: %s\n", SDL_GetError());
        SDL_DestroyWindow(win->window);
        free(win);
        return NULL;
    }

#ifdef __APPLE__
    /* macOSフルスクリーン中のSpace切り替え対策。 */
    void *nswindow = SDL_GetPointerProperty(SDL_GetWindowProperties(win->window),
                                            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    pal_macos_set_fullscreen_auxiliary(nswindow);
#endif

    g_aux_windows[slot] = win;
    return win;
}

void pal_aux_window_destroy(pal_aux_window_t *win) {
    if (!win)
        return;
    for (int i = 0; i < PAL_AUX_WINDOW_MAX; i++)
        if (g_aux_windows[i] == win)
            g_aux_windows[i] = NULL;
    if (win->renderer)
        SDL_DestroyRenderer(win->renderer);
    if (win->window)
        SDL_DestroyWindow(win->window);
    free(win);
}

int pal_aux_window_take_close_request(pal_aux_window_t *win) {
    if (!win)
        return 0;
    int r = win->close_requested;
    win->close_requested = 0;
    return r;
}

int pal_aux_window_take_click(pal_aux_window_t *win, int *out_x, int *out_y) {
    if (!win || !win->click_pending)
        return 0;
    win->click_pending = 0;
    if (out_x)
        *out_x = win->click_x;
    if (out_y)
        *out_y = win->click_y;
    return 1;
}

int pal_aux_window_is_pressed(pal_aux_window_t *win, int *out_x, int *out_y) {
    if (!win || !win->mouse_down)
        return 0;
    if (out_x)
        *out_x = win->mouse_x;
    if (out_y)
        *out_y = win->mouse_y;
    return 1;
}

void pal_aux_window_set_key_forward(pal_aux_window_t *win, int enable) {
    if (!win)
        return;
    win->key_forward_enabled = enable;
    if (!enable)
        memset(win->held_kcode_bits, 0, sizeof(win->held_kcode_bits));
}

int pal_aux_window_is_kcode_pressed(pal_aux_window_t *win, uint8_t kcode) {
    if (!win || kcode >= 0x80)
        return 0;
    return (win->held_kcode_bits[kcode >> 5] >> (kcode & 31)) & 1u;
}

int pal_rect_contains(const pal_rect_t *r, int px, int py) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

static void aux_set_draw_color(SDL_Renderer *renderer, uint32_t argb) {
    SDL_SetRenderDrawColor(renderer, (uint8_t)(argb >> 16), (uint8_t)(argb >> 8), (uint8_t)argb,
                           (uint8_t)(argb >> 24));
}

void pal_aux_window_fill_rect(pal_aux_window_t *win, const pal_rect_t *r, uint32_t argb) {
    if (!win)
        return;
    aux_set_draw_color(win->renderer, argb);
    SDL_FRect fr = {(float)r->x, (float)r->y, (float)r->w, (float)r->h};
    SDL_RenderFillRect(win->renderer, &fr);
}

void pal_aux_window_draw_char(pal_aux_window_t *win, int x, int y, char ch,
                              const uint8_t *cgrom_ank, uint32_t argb) {
    if (!win)
        return;
    aux_set_draw_color(win->renderer, argb);
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            if (pal_aux_ank_glyph_pixel(cgrom_ank, (uint8_t)ch, col, row))
                SDL_RenderPoint(win->renderer, (float)(x + col), (float)(y + row));
        }
    }
}

void pal_aux_window_draw_text(pal_aux_window_t *win, int x, int y, const char *s,
                              const uint8_t *cgrom_ank, uint32_t argb) {
    int cx = x;
    for (const char *p = s; *p; p++) {
        pal_aux_window_draw_char(win, cx, y, *p, cgrom_ank, argb);
        cx += 8;
    }
}

/* 補助ウィンドウ診断ダンプ: X68K_AUX_WINDOW_DUMP=<path>指定時、
 * present()の都度そのフレームの内容をPPM(P6)へ書き出す（同名ファイルを
 * 上書き、常に最新フレームを反映）。MCP screenshotツールはTVRAM合成専用で
 * 補助ウィンドウを撮れないため、X68K_AUDIO_DUMPと同じ「既定オフの診断
 * 環境変数」方式で目視確認できるようにする。複数の補助ウィンドウが
 * 同時に存在する場合は最後にpresent()されたものが残る。 */
static void aux_window_dump_if_requested(pal_aux_window_t *win) {
    static int checked = 0;
    static const char *path = NULL;
    if (!checked) {
        checked = 1;
        path = getenv("X68K_AUX_WINDOW_DUMP");
        if (path)
            pal_log("X68K_AUX_WINDOW_DUMP: %s へ補助ウィンドウのPPMを書き出します\n", path);
    }
    if (!path)
        return;
    SDL_Surface *surf = SDL_RenderReadPixels(win->renderer, NULL);
    if (!surf)
        return;
    SDL_Surface *rgb = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGB24);
    SDL_DestroySurface(surf);
    if (!rgb)
        return;
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", rgb->w, rgb->h);
        for (int y = 0; y < rgb->h; y++)
            fwrite((uint8_t *)rgb->pixels + y * rgb->pitch, 1, (size_t)rgb->w * 3, fp);
        fclose(fp);
    }
    SDL_DestroySurface(rgb);
}

void pal_aux_window_present(pal_aux_window_t *win) {
    if (!win)
        return;
    /* ダンプはpresent（バッファフリップ）より前に読む必要がある
     * （フリップ後はSDL_RenderReadPixelsが未定義内容を返しうるため）。 */
    aux_window_dump_if_requested(win);
    SDL_RenderPresent(win->renderer);
}

pal_aux_hotkey_t pal_aux_window_take_hotkey(void) {
    pal_aux_hotkey_t k = g_aux_hotkey_req;
    g_aux_hotkey_req = PAL_AUX_HOTKEY_NONE;
    return k;
}

/* メニューバー「File > Open Disk Selector...」選択時にメニュー側
 * (macOS: platform_macos_aux.m / Windows: platform_win_aux.c / Linux:
 * platform_linux_aux.c)から呼ばれる。
 * F12ホットキーと同じ経路（g_aux_hotkey_req、pal_aux_window_take_hotkey()で
 * 消費）でディスク選択ウィンドウを開かせる。 */
void pal_aux_menu_trigger_disk_selector(void) {
    g_aux_hotkey_req = PAL_AUX_HOTKEY_DISK_WINDOW;
}

/* メニューバー「Tools > Open Software Keyboard...」選択時にネイティブメニュー
 * 側から呼ばれる。Shift+F12ホットキーと同じ経路でソフトウェアキーボード
 * ウィンドウを開かせる。 */
void pal_aux_menu_trigger_keyboard(void) {
    g_aux_hotkey_req = PAL_AUX_HOTKEY_KEYBOARD_WINDOW;
}

/* メニューバー「Help > About ix68k...」選択時にネイティブメニュー側から呼ばれる。
 * バージョンを情報ダイアログで表示する（閉じるまでメニュー呼び出し元で待つ）。 */
void pal_aux_menu_trigger_about(void) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "About ix68k",
                             "ix68k " X68K_VERSION_STRING, g_window);
}

/* メニューバー「Machine > Reset」選択時にネイティブメニュー側から呼ばれる。
 * 実際の CPU/バスリセット処理は
 * main.cのメインループがPAL_AUX_HOTKEY_RESETを見て行う。 */
void pal_aux_menu_trigger_reset(void) {
    g_aux_hotkey_req = PAL_AUX_HOTKEY_RESET;
}

/* メニューバー「Machine > Reset (Force FD0 Boot)」選択時にネイティブメニュー側
 * から呼ばれる。実際のリセット・FD0強制起動処理はmain.cのメインループが
 * このホットキーを見て行う。 */
void pal_aux_menu_trigger_reset_fd0(void) {
    g_aux_hotkey_req = PAL_AUX_HOTKEY_RESET_FD0;
}

/* メニュー「View」(x1/x1.5/x2)(x1/x1.5/x2)選択時にネイティブメニュー側から
 * 呼ばれる。main.cのメインループがPAL_AUX_HOTKEY_ZOOM_*を見てウィンドウ
 * サイズを再計算し、pal_aux_set_current_window_zoom_x10()でチェックマークを
 * 更新する。 */
void pal_aux_menu_trigger_zoom_1(void) {
    g_aux_hotkey_req = PAL_AUX_HOTKEY_ZOOM_1;
}
void pal_aux_menu_trigger_zoom_1_5(void) {
    g_aux_hotkey_req = PAL_AUX_HOTKEY_ZOOM_1_5;
}
void pal_aux_menu_trigger_zoom_2(void) {
    g_aux_hotkey_req = PAL_AUX_HOTKEY_ZOOM_2;
}

/* 「View」メニューを開く直前にネイティブメニュー側が
 * チェックマーク表示のために呼ぶ。 */
int pal_aux_menu_current_window_zoom_x10(void) {
    return g_current_window_zoom_x10;
}

/* メニュー「View > Fullscreen」選択時にネイティブメニュー側から
 * 呼ばれる。F11と同じ経路（g_aux_hotkey_req、pal_aux_window_take_hotkey()で
 * 消費）でmain.cにフルスクリーントグルを行わせる。 */
void pal_aux_menu_trigger_fullscreen_toggle(void) {
    g_aux_hotkey_req = PAL_AUX_HOTKEY_FULLSCREEN_TOGGLE;
}

/* ---- ネイティブファイルダイアログ ---- */

/* SDL_ShowOpenFileDialog()のコールバックは別スレッドから呼ばれる場合が
 * あるため(SDL3公式ドキュメント記載)、mutexで保護したメールボックス
 * 経由でメインスレッドへ結果を渡す。本プロジェクトは他に一切スレッドを
 * 使わない設計のため、bus状態への直接アクセスはメインループ側の
 * pal_aux_window_take_file_dialog_result()経由に限定する。 */
static SDL_Mutex *g_dialog_mutex = NULL;
static char g_dialog_result_path[1024];
static int g_dialog_state = 0; /* 0=結果なし 1=選択済み 2=キャンセル/エラー */

static void dialog_callback(void *userdata, const char *const *filelist, int filter) {
    (void)userdata;
    (void)filter;
    /* filelist==NULL はエラー（ダイアログ手段が無い等。Linux は zenity か xdg-desktop-portal が
     * 要る）。空リスト（filelist[0]==NULL）はユーザーのキャンセル。区別せずに黙るとボタンが
     * 無反応に見えるため、エラーだけログへ出す。 */
    if (!filelist)
        pal_log("file dialog: 開けませんでした: %s\n", SDL_GetError());
    SDL_LockMutex(g_dialog_mutex);
    if (!filelist || !filelist[0]) {
        g_dialog_state = 2;
    } else {
        strncpy(g_dialog_result_path, filelist[0], sizeof(g_dialog_result_path) - 1);
        g_dialog_result_path[sizeof(g_dialog_result_path) - 1] = '\0';
        g_dialog_state = 1;
    }
    SDL_UnlockMutex(g_dialog_mutex);
}

void pal_aux_window_open_file_dialog(pal_aux_window_t *win, const char *filter_pattern) {
    if (!g_dialog_mutex)
        g_dialog_mutex = SDL_CreateMutex();
    SDL_DialogFileFilter filters[1];
    int nfilters = 0;
    if (filter_pattern) {
        filters[0].name = "Disk Image";
        filters[0].pattern = filter_pattern;
        nfilters = 1;
    }
    SDL_ShowOpenFileDialog(dialog_callback, NULL, win ? win->window : NULL,
                           nfilters ? filters : NULL, nfilters, NULL, false);
}

int pal_aux_window_take_file_dialog_result(char *out_path, int out_path_cap) {
    if (!g_dialog_mutex)
        return 0;
    SDL_LockMutex(g_dialog_mutex);
    int state = g_dialog_state;
    if (state == 1 && out_path && out_path_cap > 0) {
        strncpy(out_path, g_dialog_result_path, (size_t)out_path_cap - 1);
        out_path[out_path_cap - 1] = '\0';
    }
    g_dialog_state = 0;
    SDL_UnlockMutex(g_dialog_mutex);
    return state;
}
