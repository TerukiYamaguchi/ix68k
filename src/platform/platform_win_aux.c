/*
 * Windows版のネイティブメニューバー実装。macOSのCocoaメニューバー
 * （platform_macos_aux.m）に相当する File/Machine/View/Tools メニューを
 * Win32のHMENUで再現する。SDL3自体はクロスプラットフォームAPIのため
 * メニューバーを持たない。
 *
 * 既定の配色（白地）は見にくいとの指摘を受け、
 * 全項目をMF_OWNERDRAWにして背景をグレー・文字を黒で自前描画する。
 * これに伴い、WM_MEASUREITEM/WM_DRAWITEM（ウィンドウプロシージャへ直接
 * SendMessageされ、SDLのメッセージループを経由しないためSDL_SetWindowsMessage
 * Hookでは横取りできない）も扱う必要があるため、SDLのウィンドウプロシージャを
 * 素のWin32サブクラス化（GWLP_WNDPROC差し替え）で横取りする。メニュー選択
 * (WM_COMMAND)・サブメニューを開く直前のチェックマーク更新(WM_INITMENUPOPUP)
 * もこのサブクラスプロシージャ経由に統一する。
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#include "platform/debugger_launcher.h"
#include "platform/platform.h"
#include "platform/platform_win_aux.h"

enum {
    ID_FILE_OPEN_DISK = 1001,
    ID_FILE_OPEN_DATA_FOLDER = 1002,

    ID_MACHINE_INTERRUPT = 1010,
    ID_MACHINE_RESET = 1011,
    ID_MACHINE_RESET_FD0 = 1012,
    ID_MACHINE_POWER_OFF = 1013,

    ID_TOOLS_OPEN_KEYBOARD = 1030,
    ID_TOOLS_OPEN_DEBUGGER = 1031,

    ID_HELP_ABOUT = 1040,

    ID_ZOOM_1 = 1020,
    ID_ZOOM_1_5 = 1021,
    ID_ZOOM_2 = 1022,
    ID_FULLSCREEN = 1023,
};

/* オーナードロー配色。既定の白背景が見にくいとの依頼により、黒文字が
 * くっきり読める明るさのグレーに固定する（Windowsのテーマ設定に依存させず
 * 常にこの配色にする）。 */
#define MENU_COLOR_BG       RGB(220, 220, 220)
#define MENU_COLOR_TEXT     RGB(0, 0, 0)
#define MENU_COLOR_DISABLED RGB(150, 150, 150)
#define MENU_COLOR_SEP      RGB(160, 160, 160)

/* WM_INITMENUPOPUPで渡されるHMENUがどのサブメニューかを判定するために保持。
 * s_menu_barはWM_DRAWITEMでメニューバー本体の項目(File等)とポップアップ内の
 * 項目を描き分けるために使う。二重インストール防止にも使う（NULLなら
 * 未インストール）。 */
static HMENU s_menu_bar = NULL;
static HMENU s_view_menu = NULL;
static HMENU s_tools_menu = NULL;
static HFONT s_menu_font = NULL;
static HBRUSH s_menu_bg_brush = NULL;
static WNDPROC s_original_wndproc = NULL;

/* メニューバー本体の背景（File項目の右側、項目が無い余白部分）は
 * オーナードロー項目の塗りが及ばず、既定では白いまま残る。MENUINFOの
 * hbrBackでメニュー自体の背景ブラシを指定し、その余白もグレーにする。 */
static void apply_menu_background(HMENU menu) {
    if (!s_menu_bg_brush)
        s_menu_bg_brush = CreateSolidBrush(MENU_COLOR_BG);
    MENUINFO mi;
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = s_menu_bg_brush;
    SetMenuInfo(menu, &mi);
}

/* メニュー項目の描画に使うフォントをメニューバーと揃える（1度だけ生成し、
 * アプリ生存中ずっと使い回す。macOS版のs_menu_handlerと同じく明示的な
 * 解放はしない）。 */
static void ensure_menu_font(void) {
    if (s_menu_font)
        return;
    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        s_menu_font = CreateFontIndirectW(&ncm.lfMenuFont);
    if (!s_menu_font)
        s_menu_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

/* 「View」メニューを開く直前(WM_INITMENUPOPUP)に、現在の倍率へ
 * チェックマークを付け直し、フルスクリーン中は倍率項目を無効化する
 * （macOS版と同じくフルスクリーン中はウィンドウサイズという概念が無意味）。 */
static void refresh_window_size_menu_checks(void) {
    int zoom_x10 = pal_aux_menu_current_window_zoom_x10();
    int fullscreen = pal_is_fullscreen();
    static const struct {
        UINT id;
        int zoom_x10;
    } items[] = {
        {ID_ZOOM_1, 10},
        {ID_ZOOM_1_5, 15},
        {ID_ZOOM_2, 20},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        CheckMenuItem(s_view_menu, items[i].id,
                      MF_BYCOMMAND |
                          (items[i].zoom_x10 == zoom_x10 ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(s_view_menu, items[i].id,
                       MF_BYCOMMAND | (fullscreen ? MF_GRAYED : MF_ENABLED));
    }
    CheckMenuItem(s_view_menu, ID_FULLSCREEN,
                  MF_BYCOMMAND | (fullscreen ? MF_CHECKED : MF_UNCHECKED));
}

/* 「Tools」メニューを開く直前(WM_INITMENUPOPUP)に、起動スクリプトが無い環境
 * （配布物等）では「Open Debugger...」を無効化する。 */
static void refresh_tools_menu_debugger_item(void) {
    EnableMenuItem(s_tools_menu, ID_TOOLS_OPEN_DEBUGGER,
                   MF_BYCOMMAND | (pal_debugger_available() ? MF_ENABLED : MF_GRAYED));
}

/* メニュー項目選択(WM_COMMAND、LOWORD(wParam)=項目ID)に応じて対応する
 * pal_aux_menu_trigger_*()を呼ぶ。範囲外のIDは無視する（メニュー以外の
 * WM_COMMANDは発生しない想定だが、念のため）。 */
static void dispatch_menu_command(WORD id) {
    switch (id) {
    case ID_FILE_OPEN_DISK:
        pal_aux_menu_trigger_disk_selector();
        break;
    case ID_TOOLS_OPEN_KEYBOARD:
        pal_aux_menu_trigger_keyboard();
        break;
    case ID_HELP_ABOUT:
        pal_aux_menu_trigger_about();
        break;
    case ID_MACHINE_INTERRUPT:
        pal_aux_menu_trigger_interrupt();
        break;
    case ID_MACHINE_RESET:
        pal_aux_menu_trigger_reset();
        break;
    case ID_MACHINE_RESET_FD0:
        pal_aux_menu_trigger_reset_fd0();
        break;
    case ID_MACHINE_POWER_OFF:
        pal_aux_menu_trigger_power_switch_off();
        break;
    case ID_FILE_OPEN_DATA_FOLDER:
        /* 失敗理由は pal_open_user_dir() が pal_log() に出す。 */
        (void)pal_open_user_dir();
        break;
    case ID_TOOLS_OPEN_DEBUGGER:
        /* 失敗理由と二重起動の通知は pal_debugger_launch() が pal_log() に出す。 */
        (void)pal_debugger_launch();
        break;
    case ID_ZOOM_1:
        pal_aux_menu_trigger_zoom_1();
        break;
    case ID_ZOOM_1_5:
        pal_aux_menu_trigger_zoom_1_5();
        break;
    case ID_ZOOM_2:
        pal_aux_menu_trigger_zoom_2();
        break;
    case ID_FULLSCREEN:
        pal_aux_menu_trigger_fullscreen_toggle();
        break;
    default:
        break;
    }
}

/* WM_MEASUREITEM(ODT_MENU): 項目のテキスト(itemData、区切り線はNULL)を
 * メニューフォントで実測してitemWidth/itemHeightを決める。 */
static void handle_measure_item(MEASUREITEMSTRUCT *mis) {
    if (mis->CtlType != ODT_MENU)
        return;
    ensure_menu_font();
    const wchar_t *text = (const wchar_t *)mis->itemData;
    if (!text) {
        /* 区切り線。幅はポップアップ内の他項目中最大のものへ自動的に
         * 引き伸ばされるため、ここでは最小限でよい。 */
        mis->itemWidth = 8;
        mis->itemHeight = 7;
        return;
    }
    HDC hdc = GetDC(NULL);
    HFONT old_font = (HFONT)SelectObject(hdc, s_menu_font);
    SIZE sz;
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    SelectObject(hdc, old_font);
    ReleaseDC(NULL, hdc);
    /* 左のチェックマーク欄・左右の余白ぶん広めに確保する。 */
    mis->itemWidth = (UINT)(sz.cx + 32);
    mis->itemHeight = (UINT)(sz.cy + 8);
}

/* WM_DRAWITEM(ODT_MENU): 背景をグレー、文字を黒で描く。選択中は
 * システムのハイライト色（視認性のため配色はそのまま踏襲）、無効項目は
 * 淡いグレー文字にする。区切り線はitemDataがNULLの項目として判別する。
 * dis->hwndItemはODT_MENUの場合HWNDではなくHMENU（所属するメニューの
 * ハンドル）が入るWin32の仕様を利用し、メニューバー本体(File等)の項目かどうかを
 * 判定して中央寄せ/左寄せを切り替える。 */
static void handle_draw_item(const DRAWITEMSTRUCT *dis) {
    if (dis->CtlType != ODT_MENU)
        return;
    ensure_menu_font();

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const wchar_t *text = (const wchar_t *)dis->itemData;
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    bool checked = (dis->itemState & ODS_CHECKED) != 0;
    bool is_bar_item = ((HMENU)dis->hwndItem == s_menu_bar);

    COLORREF bg = selected ? GetSysColor(COLOR_HIGHLIGHT) : MENU_COLOR_BG;
    COLORREF fg = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                           : (disabled ? MENU_COLOR_DISABLED : MENU_COLOR_TEXT);

    HBRUSH bg_brush = CreateSolidBrush(bg);
    FillRect(hdc, &rc, bg_brush);
    DeleteObject(bg_brush);

    if (!text) {
        /* 区切り線: 背景中央に1pxの線を引くだけ。 */
        RECT line = rc;
        line.top = (rc.top + rc.bottom) / 2;
        line.bottom = line.top + 1;
        HBRUSH line_brush = CreateSolidBrush(MENU_COLOR_SEP);
        FillRect(hdc, &line, line_brush);
        DeleteObject(line_brush);
        return;
    }

    HFONT old_font = (HFONT)SelectObject(hdc, s_menu_font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);

    if (is_bar_item) {
        DrawTextW(hdc, text, -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    } else {
        RECT text_rc = rc;
        text_rc.left += 24; /* チェックマーク欄ぶんの余白 */
        DrawTextW(hdc, text, -1, &text_rc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        if (checked) {
            RECT chk_rc = rc;
            chk_rc.right = chk_rc.left + 24;
            DrawTextW(hdc, L"✓", -1, &chk_rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        }
    }
    SelectObject(hdc, old_font);
}

/* SDLが生成したウィンドウのWNDPROCを差し替えるサブクラスプロシージャ。
 * メニュー関連(WM_COMMAND/WM_INITMENUPOPUP/WM_MEASUREITEM/WM_DRAWITEM)
 * だけをここで処理し、それ以外は元のプロシージャ(s_original_wndproc)へ
 * そのまま委譲する。WM_MEASUREITEM/WM_DRAWITEMはWindowsがウィンドウ
 * プロシージャへ直接SendMessageするため、SDL_SetWindowsMessageHook
 * （TranslateMessage前のメッセージループ横取り）では捕捉できず、この
 * サブクラス化が必須。 */
static LRESULT CALLBACK pal_win_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                              LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (lParam == 0) { /* メニュー選択（コントロール通知はlParamにHWNDが入る） */
            dispatch_menu_command(LOWORD(wParam));
            return 0;
        }
        break;
    case WM_INITMENUPOPUP: {
        HMENU popup = (HMENU)wParam;
        if (popup == s_view_menu)
            refresh_window_size_menu_checks();
        else if (popup == s_tools_menu)
            refresh_tools_menu_debugger_item();
        return 0;
    }
    case WM_MEASUREITEM:
        handle_measure_item((MEASUREITEMSTRUCT *)lParam);
        return TRUE;
    case WM_DRAWITEM:
        handle_draw_item((const DRAWITEMSTRUCT *)lParam);
        return TRUE;
    default:
        break;
    }
    return CallWindowProcW(s_original_wndproc, hwnd, msg, wParam, lParam);
}

/* file_menuへMF_OWNERDRAWの通常項目を追加する。lpNewItemはオーナードロー
 * 項目ではテキストポインタではなくitemData（WM_MEASUREITEM/WM_DRAWITEMの
 * itemDataに渡る値）として扱われる仕様を利用し、表示文字列そのものを
 * そのまま渡す（文字列リテラルは静的領域にあるため寿命は問題ない）。 */
static void append_owner_draw_item(HMENU menu, UINT id, const wchar_t *text) {
    AppendMenuW(menu, MF_OWNERDRAW, id, text);
}

static void append_owner_draw_separator(HMENU menu) {
    AppendMenuW(menu, MF_OWNERDRAW | MF_SEPARATOR, 0, NULL);
}

static void append_owner_draw_popup(HMENU menu, HMENU submenu, const wchar_t *text) {
    AppendMenuW(menu, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)submenu, text);
}

void pal_win_install_menu(SDL_Window *window) {
    if (s_menu_bar)
        return; /* 二重インストール防止 */

    void *hwnd_prop = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd_prop)
        return;
    HWND hwnd = (HWND)hwnd_prop;

    /* SetMenu()はアタッチした瞬間に全オーナードロー項目へWM_MEASUREITEMを
     * 送る。この時点でまだ元のSDLのWNDPROCのままだとオーナードローの
     * サイズ計算が一切行われず、以後のWM_DRAWITEMがゼロに近いサイズで
     * 描く破綻したレイアウトになる。そのため、メニュー構築・SetMenu()より
     * 先にサブクラス化しておく。 */
    s_original_wndproc =
        (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)pal_win_subclass_proc);

    s_menu_bar = CreateMenu();

    /* File: ディスク・データフォルダ。 */
    HMENU file_menu = CreatePopupMenu();
    append_owner_draw_item(file_menu, ID_FILE_OPEN_DISK, L"Open Disk Selector...");
    append_owner_draw_separator(file_menu);
    /* Open Data Folder: 設定・ROM・ディスク・SRAMの置き場所（%APPDATA%\ix68k\）を
     * エクスプローラーで開く。 */
    append_owner_draw_item(file_menu, ID_FILE_OPEN_DATA_FOLDER, L"Open Data Folder");

    /* Machine: 実機相当の操作。 */
    HMENU machine_menu = CreatePopupMenu();
    append_owner_draw_item(machine_menu, ID_MACHINE_RESET, L"Reset");
    append_owner_draw_item(machine_menu, ID_MACHINE_RESET_FD0, L"Reset (Force FD0 Boot)");
    append_owner_draw_separator(machine_menu);
    append_owner_draw_item(machine_menu, ID_MACHINE_INTERRUPT, L"Interrupt");
    append_owner_draw_item(machine_menu, ID_MACHINE_POWER_OFF, L"Power Switch (OFF)");

    /* View: ウィンドウサイズ。256x256等の正方形ドットモードでネイティブ解像度が
     * 小さくなりウィンドウが縮んでしまう問題への対処。SRAMには保存しない
     * （実機にない機能のため）。現在値にはWM_INITMENUPOPUPでチェックマークを
     * 表示する。 */
    s_view_menu = CreatePopupMenu();
    append_owner_draw_item(s_view_menu, ID_ZOOM_1, L"x1 (Native)");
    append_owner_draw_item(s_view_menu, ID_ZOOM_1_5, L"x1.5");
    append_owner_draw_item(s_view_menu, ID_ZOOM_2, L"x2");
    append_owner_draw_separator(s_view_menu);
    append_owner_draw_item(s_view_menu, ID_FULLSCREEN, L"Fullscreen");

    /* Tools: ソフトウェアキーボードと外部デバッガ ix68k-debugger（tools-host\debugger\）。
     * デバッガは開発ツリー向けで、起動スクリプトが無い環境ではWM_INITMENUPOPUPで
     * 無効化する。 */
    s_tools_menu = CreatePopupMenu();
    append_owner_draw_item(s_tools_menu, ID_TOOLS_OPEN_KEYBOARD, L"Open Software Keyboard...");
    append_owner_draw_item(s_tools_menu, ID_TOOLS_OPEN_DEBUGGER, L"Open Debugger...");

    append_owner_draw_popup(s_menu_bar, file_menu, L"File");
    append_owner_draw_popup(s_menu_bar, machine_menu, L"Machine");
    append_owner_draw_popup(s_menu_bar, s_view_menu, L"View");
    append_owner_draw_popup(s_menu_bar, s_tools_menu, L"Tools");
    /* Help: バージョンを表示する About ダイアログ。 */
    HMENU help_menu = CreatePopupMenu();
    append_owner_draw_item(help_menu, ID_HELP_ABOUT, L"About ix68k...");
    append_owner_draw_popup(s_menu_bar, help_menu, L"Help");
    apply_menu_background(help_menu);
    apply_menu_background(s_menu_bar);
    apply_menu_background(file_menu);
    apply_menu_background(machine_menu);
    apply_menu_background(s_view_menu);
    apply_menu_background(s_tools_menu);
    SetMenu(hwnd, s_menu_bar);
}
