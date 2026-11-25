/*
 * Linux(X11/Wayland、SDL3)版の File/Machine/View/Tools メニュー実装。
 * platform_linux_aux.h参照。macOS/WindowsのようなネイティブメニューAPIが
 * 無いため、全て自前描画・自前ヒットテストで実装する（CGROM ANKフォントは
 * aux_window.c（ディスク選択/ソフトウェアキーボード）と同じ
 * pal_aux_ank_glyph_pixel()を使う）。
 *
 * メニューバー本体（4つのボタン）はpal_menu_bar_height()ぶんウィンドウが常時確保する
 * 領域に常駐して毎フレーム描く。ドロップダウンは開いている間
 * だけ、合成済みのX68000画面の上に重ねて描く（ネイティブOSのメニューが
 * クライアント領域に一時的に重なって開くのと同じ挙動）。
 */
#include "platform/debugger_launcher.h"
#include "platform/platform_sdl3_internal.h"
#include "platform/platform_linux_aux.h"
#include "platform/aux_font.h"
#include <stddef.h>

#define ROW_H       PAL_LINUX_MENU_BAR_H
#define CHAR_W      8
#define GLYPH_H     16
#define CHECK_COL_W 20
#define BTN_X0      4
#define BTN_W       72
#define DROPDOWN_Y  PAL_LINUX_MENU_BAR_H
#define DROPDOWN_W  260

#define COLOR_BG          0xFFDCDCDCu /* 220,220,220 */
#define COLOR_TEXT        0xFF000000u
#define COLOR_SEP         0xFFA0A0A0u /* 160,160,160 */
#define COLOR_DISABLED    0xFF969696u /* 150,150,150 */
#define COLOR_BORDER      0xFF808080u /* 128,128,128 */
#define COLOR_HILITE      0xFF3380E6u
#define COLOR_HILITE_TEXT 0xFFFFFFFFu

/* 開いているメニューの添字（menu_id_t）。閉じている間は MENU_NONE。 */
typedef enum {
    MENU_NONE = -1,
    MENU_FILE = 0,
    MENU_MACHINE,
    MENU_VIEW,
    MENU_TOOLS,
    MENU_HELP,
    MENU_COUNT,
} menu_id_t;

typedef enum {
    ACT_NONE = 0,
    ACT_OPEN_DISK,
    ACT_OPEN_DATA_FOLDER,
    ACT_RESET,
    ACT_RESET_FD0,
    ACT_INTERRUPT,
    ACT_POWER_OFF,
    ACT_OPEN_KEYBOARD,
    ACT_OPEN_DEBUGGER,
    ACT_ZOOM_1,
    ACT_ZOOM_1_5,
    ACT_ZOOM_2,
    ACT_FULLSCREEN,
    ACT_ABOUT,
} menu_action_t;

typedef struct {
    const char *label; /* NULLなら区切り線 */
    menu_action_t action;
} menu_item_t;

static const menu_item_t kFileItems[] = {
    {"Open Disk Selector...", ACT_OPEN_DISK},
    {NULL, ACT_NONE},
    {"Open Data Folder", ACT_OPEN_DATA_FOLDER},
};

static const menu_item_t kMachineItems[] = {
    {"Reset", ACT_RESET},
    {"Reset (Force FD0 Boot)", ACT_RESET_FD0},
    {NULL, ACT_NONE},
    {"Interrupt", ACT_INTERRUPT},
    {"Power Switch (OFF)", ACT_POWER_OFF},
};

static const menu_item_t kViewItems[] = {
    {"x1 (Native)", ACT_ZOOM_1},
    {"x1.5", ACT_ZOOM_1_5},
    {"x2", ACT_ZOOM_2},
    {NULL, ACT_NONE},
    {"Fullscreen", ACT_FULLSCREEN},
};

static const menu_item_t kToolsItems[] = {
    {"Open Software Keyboard...", ACT_OPEN_KEYBOARD},
    {"Open Debugger...", ACT_OPEN_DEBUGGER},
};

static const menu_item_t kHelpItems[] = {
    {"About ix68k...", ACT_ABOUT},
};

typedef struct {
    const char *title;
    const menu_item_t *items;
    int count;
} menu_def_t;

#define ITEM_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* menu_id_t と同じ並び（添字がそのままボタンの左からの順序）。 */
static const menu_def_t kMenus[MENU_COUNT] = {
    {"File", kFileItems, ITEM_COUNT(kFileItems)},
    {"Machine", kMachineItems, ITEM_COUNT(kMachineItems)},
    {"View", kViewItems, ITEM_COUNT(kViewItems)},
    {"Tools", kToolsItems, ITEM_COUNT(kToolsItems)},
    {"Help", kHelpItems, ITEM_COUNT(kHelpItems)},
};

static menu_id_t s_open_menu = MENU_NONE;
static const uint8_t *s_cgrom_ank = NULL;
static int s_mouse_x = -1, s_mouse_y = -1;

void pal_linux_menu_set_cgrom(const uint8_t *cgrom_ank) {
    s_cgrom_ank = cgrom_ank;
}

/* ---- 描画ヘルパー（aux_window.cのpal_aux_window_fill_rect/draw_text相当を
 * g_renderer向けに持つ。pal_aux_window_tに紐付くAPIはplatform.hの公開
 * インターフェースでありSDL非依存を保つ必要があるため、ここでは流用せず
 * 同等の処理を直接書く）。 ---- */

static void set_color(uint32_t argb) {
    SDL_SetRenderDrawColor(g_renderer, (uint8_t)(argb >> 16), (uint8_t)(argb >> 8),
                           (uint8_t)argb, (uint8_t)(argb >> 24));
}

static void fill_rect(SDL_FRect r, uint32_t argb) {
    set_color(argb);
    SDL_RenderFillRect(g_renderer, &r);
}

static void draw_border(SDL_FRect r, uint32_t argb) {
    set_color(argb);
    SDL_RenderRect(g_renderer, &r);
}

static void draw_char(int x, int y, char ch, uint32_t argb) {
    if (!s_cgrom_ank)
        return;
    set_color(argb);
    for (int row = 0; row < GLYPH_H; row++) {
        for (int col = 0; col < 8; col++) {
            if (pal_aux_ank_glyph_pixel(s_cgrom_ank, (uint8_t)ch, col, row))
                SDL_RenderPoint(g_renderer, (float)(x + col), (float)(y + row));
        }
    }
}

static void draw_text(int x, int y, const char *s, uint32_t argb) {
    int cx = x;
    for (const char *p = s; *p; p++) {
        draw_char(cx, y, *p, argb);
        cx += CHAR_W;
    }
}

static void draw_text_centered(SDL_FRect r, const char *s, uint32_t argb) {
    int len = 0;
    for (const char *p = s; *p; p++)
        len++;
    int x = (int)(r.x + (r.w - (float)(len * CHAR_W)) / 2.0f);
    int y = (int)(r.y + (r.h - (float)GLYPH_H) / 2.0f);
    draw_text(x, y, s, argb);
}

static int point_in(SDL_FRect r, int x, int y) {
    return x >= r.x && (float)x < r.x + r.w && y >= r.y && (float)y < r.y + r.h;
}

/* ---- レイアウト ---- */

static SDL_FRect menu_button_rect(menu_id_t id) {
    SDL_FRect r = {(float)(BTN_X0 + (int)id * BTN_W), 0.0f, (float)BTN_W, (float)ROW_H};
    return r;
}

/* ドロップダウンは該当ボタンの真下、ボタンと左端を揃えて開く。 */
static SDL_FRect dropdown_rect(menu_id_t id) {
    SDL_FRect r = {menu_button_rect(id).x, (float)DROPDOWN_Y, (float)DROPDOWN_W,
                   (float)(kMenus[id].count * ROW_H)};
    return r;
}

/* マウス位置がメニューバー上のボタンに当たっていればその menu_id_t、無ければ MENU_NONE。 */
static menu_id_t button_at(int x, int y) {
    for (int i = 0; i < MENU_COUNT; i++)
        if (point_in(menu_button_rect((menu_id_t)i), x, y))
            return (menu_id_t)i;
    return MENU_NONE;
}

static int hit_row(SDL_FRect area, int x, int y) {
    if (!point_in(area, x, y))
        return -1;
    int row = (int)(((float)y - area.y) / (float)ROW_H);
    return row;
}

/* 起動スクリプトが無い環境（配布物等）では「Open Debugger...」を選べなくする。
 * 他の項目は常に有効。 */
static int item_enabled(menu_action_t action) {
    switch (action) {
    case ACT_OPEN_DEBUGGER:
        return pal_debugger_available();
    case ACT_ZOOM_1:
    case ACT_ZOOM_1_5:
    case ACT_ZOOM_2:
        /* フルスクリーン中はウィンドウサイズという概念が無意味（macOS/Windows版と同じ）。 */
        return !pal_is_fullscreen();
    default:
        return 1;
    }
}

/* チェックマーク（'*'）を付ける項目か。倍率は現在値、Fullscreenは全画面状態に一致するもの。 */
static int item_checked(menu_action_t action) {
    switch (action) {
    case ACT_ZOOM_1:
        return pal_aux_menu_current_window_zoom_x10() == 10;
    case ACT_ZOOM_1_5:
        return pal_aux_menu_current_window_zoom_x10() == 15;
    case ACT_ZOOM_2:
        return pal_aux_menu_current_window_zoom_x10() == 20;
    case ACT_FULLSCREEN:
        return pal_is_fullscreen();
    default:
        return 0;
    }
}

/* ---- 描画本体 ---- */

static void draw_item_list(const menu_item_t *items, int count, SDL_FRect area) {
    fill_rect(area, COLOR_BG);
    draw_border(area, COLOR_BORDER);
    for (int i = 0; i < count; i++) {
        SDL_FRect row = {area.x, area.y + (float)(i * ROW_H), area.w, (float)ROW_H};
        const menu_item_t *it = &items[i];
        if (!it->label) {
            SDL_FRect line = {row.x + 4, row.y + (float)(ROW_H / 2), row.w - 8, 1.0f};
            fill_rect(line, COLOR_SEP);
            continue;
        }
        int enabled = item_enabled(it->action);
        int hovered = enabled && point_in(row, s_mouse_x, s_mouse_y);
        if (hovered)
            fill_rect(row, COLOR_HILITE);
        uint32_t text_color = !enabled  ? COLOR_DISABLED
                              : hovered ? COLOR_HILITE_TEXT
                                        : COLOR_TEXT;
        int text_y = (int)(row.y + (float)(ROW_H - GLYPH_H) / 2.0f);
        draw_text((int)row.x + CHECK_COL_W, text_y, it->label, text_color);
        if (item_checked(it->action))
            draw_char((int)row.x + 6, text_y, '*', text_color);
    }
}

void pal_linux_menu_draw_overlay(void) {
    int bar_h = pal_menu_bar_height();
    if (bar_h <= 0) {
        /* フルスクリーン等でバー自体が存在しない場合は常に閉じておく。 */
        s_open_menu = MENU_NONE;
        return;
    }

    int win_w = 0, win_h = 0;
    SDL_GetWindowSize(g_window, &win_w, &win_h);
    (void)win_h;

    fill_rect((SDL_FRect){0.0f, 0.0f, (float)win_w, (float)bar_h}, COLOR_BG);
    for (int i = 0; i < MENU_COUNT; i++) {
        SDL_FRect btn = menu_button_rect((menu_id_t)i);
        int active = (s_open_menu == (menu_id_t)i);
        if (active)
            fill_rect(btn, COLOR_HILITE);
        draw_text_centered(btn, kMenus[i].title, active ? COLOR_HILITE_TEXT : COLOR_TEXT);
    }

    if (s_open_menu == MENU_NONE)
        return;

    draw_item_list(kMenus[s_open_menu].items, kMenus[s_open_menu].count,
                   dropdown_rect(s_open_menu));
}

/* ---- イベント処理 ---- */

static void dispatch_action(menu_action_t action) {
    switch (action) {
    case ACT_OPEN_DISK:
        pal_aux_menu_trigger_disk_selector();
        break;
    case ACT_OPEN_KEYBOARD:
        pal_aux_menu_trigger_keyboard();
        break;
    case ACT_ABOUT:
        pal_aux_menu_trigger_about();
        break;
    case ACT_INTERRUPT:
        pal_aux_menu_trigger_interrupt();
        break;
    case ACT_RESET:
        pal_aux_menu_trigger_reset();
        break;
    case ACT_RESET_FD0:
        pal_aux_menu_trigger_reset_fd0();
        break;
    case ACT_POWER_OFF:
        pal_aux_menu_trigger_power_switch_off();
        break;
    case ACT_OPEN_DATA_FOLDER:
        /* 失敗理由は pal_open_user_dir() が pal_log() に出す。 */
        (void)pal_open_user_dir();
        break;
    case ACT_OPEN_DEBUGGER:
        /* 失敗理由と二重起動の通知は pal_debugger_launch() が pal_log() に出す。 */
        (void)pal_debugger_launch();
        break;
    case ACT_ZOOM_1:
        pal_aux_menu_trigger_zoom_1();
        break;
    case ACT_ZOOM_1_5:
        pal_aux_menu_trigger_zoom_1_5();
        break;
    case ACT_ZOOM_2:
        pal_aux_menu_trigger_zoom_2();
        break;
    case ACT_FULLSCREEN:
        pal_aux_menu_trigger_fullscreen_toggle();
        break;
    default:
        break;
    }
}

/* 開いているドロップダウン内の行をクリックしたら実行して閉じる。バーのボタン再クリックは
 * トグルで閉じ、別ボタンなら切り替える。どこにも当たらない外側クリックは破棄して閉じる
 * （ゲストへは転送しない）。 */
static void handle_menu_click(int x, int y) {
    const menu_def_t *def = &kMenus[s_open_menu];
    int row = hit_row(dropdown_rect(s_open_menu), x, y);
    if (row >= 0 && row < def->count) {
        const menu_item_t *it = &def->items[row];
        if (!it->label)
            return; /* 区切り線クリックは無視（開いたまま） */
        if (!item_enabled(it->action))
            return; /* 無効項目のクリックは無視（開いたまま） */
        dispatch_action(it->action);
        s_open_menu = MENU_NONE;
        return;
    }

    menu_id_t hit = button_at(x, y);
    s_open_menu = (hit != MENU_NONE && hit != s_open_menu) ? hit : MENU_NONE;
}

int pal_linux_menu_handle_event(const SDL_Event *e) {
    int bar_h = pal_menu_bar_height();
    if (bar_h <= 0) {
        /* フルスクリーン等でバーが存在しない間は何も消費しない
         * （開いていれば次のdraw_overlayで強制的に閉じられる）。 */
        return 0;
    }

    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (s_open_menu == MENU_NONE)
            return ((int)e->motion.y < bar_h);
        s_mouse_x = (int)e->motion.x;
        s_mouse_y = (int)e->motion.y;
        /* 開いている間はバー上の別ボタンへホバーで切り替える（ネイティブメニューと同じ）。 */
        {
            menu_id_t hover = button_at(s_mouse_x, s_mouse_y);
            if (hover != MENU_NONE)
                s_open_menu = hover;
        }
        return 1;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        s_mouse_x = (int)e->button.x;
        s_mouse_y = (int)e->button.y;
        if (s_open_menu == MENU_NONE) {
            if (s_mouse_y < bar_h) {
                s_open_menu = button_at(s_mouse_x, s_mouse_y);
                return 1;
            }
            return 0;
        }
        handle_menu_click(s_mouse_x, s_mouse_y);
        return 1;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (s_open_menu == MENU_NONE)
            return ((int)e->button.y < bar_h);
        return 1;

    case SDL_EVENT_KEY_DOWN:
        if (s_open_menu == MENU_NONE)
            return 0;
        if (e->key.key == SDLK_ESCAPE)
            s_open_menu = MENU_NONE;
        return 1;

    case SDL_EVENT_KEY_UP:
        return s_open_menu != MENU_NONE;

    default:
        return 0;
    }
}
