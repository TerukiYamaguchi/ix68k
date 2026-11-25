#include "platform/platform_sdl3_internal.h"
#include "platform/aux_font.h"
#include "platform/platform_aux_menu.h"
#ifdef __APPLE__
#include "platform/platform_macos_aux.h"
#elif defined(_WIN32)
#include "platform/platform_win_aux.h"
#else
#include "platform/platform_linux_aux.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* pal_open_user_dir() が組み立てる file:// URL の最大長。 */
#define PAL_USER_DIR_URL_MAX 2048

SDL_Window *g_window = NULL;
SDL_Renderer *g_renderer = NULL;
static SDL_Texture *g_texture = NULL;
static int g_quit = 0;
static int g_nmi_req = 0;
static int g_power_sw_off_req = 0;
static int g_fullscreen = 0;

#ifdef __APPLE__
/* macOS(Tahoe以降)は角丸半径が大きく、SDLのコンテンツ領域がウィンドウ矩形
 * いっぱいに描かれると左下・右下の隅が角丸に食われて欠ける。
 * 角丸半径R・内側マージンpとしてp >= R(1-1/√2) ≈ 0.29Rが必要条件。
 * R=16pt相当での実測値。フルスクリーン時は角丸が無いため0にする。 */
#define PAL_CORNER_INSET 5
#endif

/* macOS角丸ウィンドウ対策の内側マージン(pt)を返す。フルスクリーン時・
 * 非macOSでは0（pal_resize/pal_present_frame/マウス座標換算で共有する）。 */
int pal_corner_inset(void) {
#ifdef __APPLE__
    if (!g_window || (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN))
        return 0;
    return PAL_CORNER_INSET;
#else
    return 0;
#endif
}

/* Linux(SDL自前描画メニュー)がウィンドウ上部に常時確保するバーの高さ(pt)。
 * macOS/Windows（ネイティブメニュー）や非GUI版では常に0。pal_corner_inset
 * と同じくフルスクリーン中は0にする（メニューを開けない・重ならせない）。 */
int pal_menu_bar_height(void) {
#if !defined(__APPLE__) && !defined(_WIN32)
    if (!g_window || (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN))
        return 0;
    return PAL_LINUX_MENU_BAR_H;
#else
    return 0;
#endif
}

int pal_init(const char *title, int w, int h) {
    /* macOSのGameController.framework(GCController、SDL_HINT_JOYSTICK_MFI)が
     * HID記述子非標準の安物パッドを「非対応」として無視しつつもデバイス自体は
     * 排他的に専有してしまい、SDLのIOKit直接バックエンドからも見えなくなる
     * ケースがあるため無効化する。SDL_Init前に設定する必要がある。 */
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    /* X11バックエンドはデフォルトでDirectColor visualをTrueColorより優先選択する
     * （gamma ramp対応のための挙動）。ix68kはgamma機能を使わないため
     * 無効化し、TrueColor visualを使わせる。WSLg(Xwayland)環境でウィンドウは
     * 生成されるが描画内容が表示されない問題の原因だった。
     * X11以外のバックエンドでは無視される。 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NODIRECTCOLOR, "1");
    if (!SDL_Init(SDL_INIT_VIDEO))
        return -1;
    /* SDL_WINDOW_RESIZABLE: リサイズ可能にすることでmacOSの緑丸(拡大)ボタンを
     * 有効化する（ネイティブフルスクリーンへの入口）。手動ドラッグでの
     * リサイズも同時に許可されるが、SDL_EVENT_WINDOW_RESIZEDでg_display_w/h
     * を追従させるので座標換算は破綻しない。
     * SDL3のSDL_CreateWindowは位置引数を取らないため、生成後に明示的に
     * 中央配置する。 */
    /* SDL_WINDOW_HIGH_PIXEL_DENSITY: CAMetalLayer.contentsScaleをRetinaの
     * 実スケールに合わせる。macOSでは、これを指定せずウィンドウの点サイズを
     * ネイティブ解像度のまま(x1ズーム)にすると、compositorがこのウィンドウを
     * 暗く表示することがある。 */
    g_window =
        SDL_CreateWindow(title, w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_window)
        return -1;
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
#ifdef __APPLE__
    /* メニューバー（File/Machine/View/Tools。Cmd+Oでディスク選択ウィンドウ等）を
     * 構築する。NSApp/mainMenu が存在するこの時点
     * （SDL_CreateWindow成功後）で一度だけ呼ぶ。 */
    pal_macos_install_open_menu_item();
    pal_macos_set_dev_dock_icon();
#elif defined(_WIN32)
    /* File/Machine/View/Toolsメニューを、Win32のネイティブメニューバー
     * として設置する。HWNDが確定するこの時点（SDL_CreateWindow成功後）で
     * 一度だけ呼ぶ。 */
    pal_win_install_menu(g_window);
#endif
    g_renderer = SDL_CreateRenderer(g_window, NULL);
    if (!g_renderer)
        return -1;
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_texture)
        return -1;
    sdl_input_init_gamepads();
    return 0;
}

void pal_quit(void) {
    sdl_input_quit();
    pal_audio_quit();
    pal_audio_capture_quit();
    pal_serial_quit();
    if (g_texture)
        SDL_DestroyTexture(g_texture);
    if (g_renderer)
        SDL_DestroyRenderer(g_renderer);
    if (g_window)
        SDL_DestroyWindow(g_window);
    SDL_Quit();
}

void pal_present_frame(const uint32_t *argb, int w, int h) {
    SDL_UpdateTexture(g_texture, NULL, argb, w * (int)sizeof(uint32_t));
    int inset = pal_corner_inset();
    int bar = pal_menu_bar_height();
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    if (inset > 0 || bar > 0) {
        /* コンテンツ矩形を角丸の内側(上端は0、左右下はinset)・Linux版メニュー
         * バーの下(上端をbarぶん下げる)に収める。ウィンドウ実サイズ(手動
         * リサイズ・DPI追従後)を毎フレーム問い合わせ、pal_resizeで加算した
         * 分を差し引いてストレッチ先とする。SDL_GetRenderOutputSizeは
         * ピクセル単位で返るため、ポイント単位のinset/barをSDL_GetWindow
         * PixelDensity()の比率でピクセル単位へ換算してから使う。 */
        int out_w, out_h;
        SDL_GetRenderOutputSize(g_renderer, &out_w, &out_h);
        float density = SDL_GetWindowPixelDensity(g_window);
        int inset_px = (int)(inset * density + 0.5f);
        int bar_px = (int)(bar * density + 0.5f);
        SDL_FRect dst = {(float)inset_px, (float)bar_px, (float)(out_w - inset_px * 2),
                         (float)(out_h - inset_px - bar_px)};
        SDL_RenderTexture(g_renderer, g_texture, NULL, &dst);
    } else {
        SDL_RenderTexture(g_renderer, g_texture, NULL, NULL);
    }
#if !defined(__APPLE__) && !defined(_WIN32)
    /* 合成済みのX68000画面の上に、Linux版メニューバー/開いていれば
     * ドロップダウンを重ねて描く。SDL_RenderPresentより前に行う必要がある。 */
    pal_linux_menu_draw_overlay();
#endif
    SDL_RenderPresent(g_renderer);
    (void)h;
}

void pal_poll_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        /* 補助ウィンドウ(キーボード/ディスク選択)宛のイベントは
         * ここで振り分けて処理し、以下の（メインウィンドウ専用の）switchには
         * 一切通さない。SDL_GetWindowFromEvent()はwindowIDを持たない
         * イベント（SDL_EVENT_QUIT等）にはNULLを返すので、その場合は
         * 素通りして従来通りメインの switch で処理される。 */
        SDL_Window *event_window = SDL_GetWindowFromEvent(&e);
        if (event_window && event_window != g_window) {
            sdl_aux_window_handle_event(event_window, &e);
            continue;
        }

#if !defined(__APPLE__) && !defined(_WIN32)
        /* Linux版メニュー（バー領域のクリック・ドロップダウン表示中の全入力）
         * 宛のイベントはここで処理し、ゲスト向けの以下のswitchには通さない。 */
        if (pal_linux_menu_handle_event(&e))
            continue;
#endif

        switch (e.type) {
        case SDL_EVENT_QUIT:
            g_quit = 1;
            break;

        /* SDLは「最後の可視ウィンドウ」が閉じられたときだけSDL_EVENT_QUITを
         * 発行する。補助ウィンドウ(キーボード/ディスク選択)が開いていると
         * メインのcloseボタンでQUITが出ないため、メインウィンドウ宛の
         * クローズ要求を直接終了要求として扱う。 */
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            g_quit = 1;
            break;

        case SDL_EVENT_KEY_DOWN: {
            SDL_Keycode sym = e.key.key;
            if (sym == SDLK_PAUSE) {
                g_nmi_req = 1;
                break;
            }
            if (sym == SDLK_F11) {
                /* 全画面トグル。メニュー「View > Fullscreen」
                 * と共通の実装(pal_toggle_fullscreen)を使う。 */
                pal_toggle_fullscreen();
                break;
            }
            pal_aux_hotkey_t hotkey_request;
            if (sdl_input_hotkey_down(sym, e.key.mod, &hotkey_request)) {
                if (hotkey_request != PAL_AUX_HOTKEY_NONE)
                    sdl_aux_window_request_hotkey(hotkey_request);
                break;
            }
            if (e.key.mod & SDL_KMOD_GUI) {
                /* Cmd+K（ソフトウェアキーボード）等、メニューのキー
                 * 等価物として処理される組み合わせでも、AppKitの
                 * performKeyEquivalent:よりSDLの生キーイベントが先に
                 * 届き、'k'がゲストへ素通ししてしまう。X68000実機に
                 * Commandキー相当は存在せずゲストへ転送する意味もない
                 * ため、GUI修飾キー押下中の入力は転送しない。 */
                break;
            }
            sdl_input_key_down(sym, e.key.mod);
            break;
        }

        case SDL_EVENT_KEY_UP: {
            if (sdl_input_hotkey_up(e.key.key))
                break;
            sdl_input_key_up(e.key.key);
            break;
        }

        /* XEiJ Mouse.java の musOnScreen 相当。ウィンドウ内にカーソルが
         * あるときだけマウスデータを送出する。macOS標準カーソルは
         * ウィンドウ内にある間だけ隠す（X68000側の描画と二重に見えるのを防ぐ）。 */
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            sdl_input_mouse_enter();
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            /* 全画面時は「画面外に出る」概念自体が不要（マルチモニタ環境等の
             * 意図しないLEAVEも無視し、常時捕捉のままにする）。 */
            sdl_input_mouse_leave();
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            /* 自前F11トグル・macOS緑丸ボタン・手動ドラッグいずれの経路でも
             * ウィンドウサイズが変わったらマウス座標換算を追従させる。 */
            sdl_input_mouse_resized();
            break;

        case SDL_EVENT_MOUSE_MOTION:
            sdl_input_mouse_motion((int)e.motion.x, (int)e.motion.y);
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            sdl_input_mouse_button();
            break;

        /* ジョイスティックのホットプラグ。SDL3 では
         * ADDED/REMOVEDどちらのe.gdevice.whichもSDL_JoystickIDで統一されている
         * （SDL2はADDED=列挙番号・REMOVED=instance IDで非対称だった）。 */
        case SDL_EVENT_GAMEPAD_ADDED:
            joystick_try_open(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            joystick_close_by_instance(e.gdevice.which);
            break;

        /* X68K_JOY_DEBUG=1時のみ。SDL_Gamepad抽象化を経由しない生入力を
         * ログ出力し、コミュニティDBマッピングが実機と合わないときの
         * 実際のボタン/軸/ハット番号を特定する。 */
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
            sdl_input_debug_joystick_event(&e);
            break;
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            sdl_input_debug_joystick_event(&e);
            break;
        case SDL_EVENT_JOYSTICK_HAT_MOTION:
            sdl_input_debug_joystick_event(&e);
            break;

        default:
            break;
        }
    }
}

int pal_should_quit(void) {
    return g_quit;
}

/* メニュー「Machine > Interrupt」選択時にネイティブメニュー側
 * (macOS: platform_macos_aux.m / Windows: platform_win_aux.c / Linux:
 * platform_linux_aux.c)から呼ばれる。 Pauseキーと同じNMI要求フラグを立てる。 */
void pal_aux_menu_trigger_interrupt(void) {
    g_nmi_req = 1;
}

/* メニュー「Machine > Power Switch (OFF)」選択時に呼ばれる。前面電源スイッチOFF要求
 *を立てる。 */
void pal_aux_menu_trigger_power_switch_off(void) {
    g_power_sw_off_req = 1;
}

/* CGROM ANKフォントをプラットフォーム層へ伝える。実際に使うのはLinux版の
 * 自前描画メニュー（platform_linux_aux.c）だけで、macOS/Windows（ネイティブ
 * メニュー）では無視する。main.cがROM/CGROMロード後に一度呼ぶ。 */
void pal_set_menu_cgrom_ank(const uint8_t *cgrom_ank) {
#if !defined(__APPLE__) && !defined(_WIN32)
    pal_linux_menu_set_cgrom(cgrom_ank);
#else
    (void)cgrom_ank;
#endif
}

int pal_nmi_requested(void) {
    int v = g_nmi_req;
    g_nmi_req = 0;
    return v;
}

int pal_power_switch_off_requested(void) {
    int v = g_power_sw_off_req;
    g_power_sw_off_req = 0;
    return v;
}

void pal_show_error(const char *title, const char *msg) {
    fprintf(stderr, "%s: %s\n", title, msg);
    fflush(stderr);
#ifdef _WIN32
    const int stderr_is_tty = _isatty(_fileno(stderr));
#else
    const int stderr_is_tty = isatty(STDERR_FILENO);
#endif
    if (!stderr_is_tty) {
        /* ダイアログを出せない環境（DISPLAY無し等）では stderr 出力だけが残る。 */
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
    }
}

void pal_resize(int native_w, int native_h, int display_w, int display_h) {
    if (!g_window || !g_renderer)
        return;
    /* 角丸対策のinsetぶんウィンドウ側を広げる（実効表示サイズは変えない）。
     * 上辺はタイトルバーがあり角丸の影響を受けないため縦は下のみ広げる。
     * Linux版のメニューバーも同様に、実効表示サイズは変えずウィンドウ上辺を
     * bar分だけ広げる（pal_present_frameで描画先をその分下へずらす）。 */
    int inset = pal_corner_inset();
    int bar = pal_menu_bar_height();
    SDL_SetWindowSize(g_window, display_w + inset * 2, display_h + inset + bar);
    sdl_input_set_dimensions(native_w, native_h, display_w, display_h);
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    /* テクスチャはネイティブ解像度で作成。SDL_RenderTexture がウィンドウサイズにストレッチする
     */
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, native_w, native_h);
}

void pal_toggle_fullscreen(void) {
    if (!g_window)
        return;
    /* SDL3は専用モード未設定時のフルスクリーンがデスクトップ解像度そのまま
     * （SDL2のFULLSCREEN_DESKTOP相当）のため、単一のSDL_WINDOW_FULLSCREEN
     * フラグで足りる。ウィンドウサイズ変更に伴うg_display_w/hの追従は
     * SDL_EVENT_WINDOW_RESIZEDで一括して行う。 */
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(g_window, g_fullscreen);
}

int pal_is_fullscreen(void) {
    return g_fullscreen;
}

const char *pal_get_exe_dir(void) {
    return SDL_GetBasePath();
}

const char *pal_get_user_dir(void) {
    static char *user_dir;
    if (!user_dir)
        user_dir = SDL_GetPrefPath("", "ix68k");
    return user_dir;
}

/* パスを file:// URL へ変換する。RFC 3986 の unreserved と '/' 以外は %XX にし、
 * Windows の '\\' は '/'、ドライブレター付きは file:///C:/... にする。
 * 成功 0、バッファ不足 -1。 */
static int path_to_file_url(const char *path, char *out, size_t out_size) {
    static const char kHex[] = "0123456789ABCDEF";
    size_t n = 0;
    const char *prefix = (path[0] == '/') ? "file://" : "file:///";
    for (const char *p = prefix; *p; p++) {
        if (n + 1 >= out_size)
            return -1;
        out[n++] = *p;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        unsigned char c = (*p == '\\') ? '/' : *p;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' ||
                    c == '/' || c == ':';
        if (n + (plain ? 1 : 3) >= out_size)
            return -1;
        if (plain) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = kHex[c >> 4];
            out[n++] = kHex[c & 0x0F];
        }
    }
    out[n] = '\0';
    return 0;
}

int pal_open_user_dir(void) {
    const char *dir = pal_get_user_dir();
    if (!dir) {
        pal_log("データフォルダを取得できません\n");
        return -1;
    }
    char url[PAL_USER_DIR_URL_MAX];
    if (path_to_file_url(dir, url, sizeof(url)) != 0) {
        pal_log("データフォルダのパスが長すぎます: %s\n", dir);
        return -1;
    }
    if (!SDL_OpenURL(url)) {
        pal_log("データフォルダを開けません: %s (%s)\n", dir, SDL_GetError());
        /* メニューからの操作なので、端末起動かどうかに関わらずダイアログで知らせる。
         * 文字化けを避けるため本文は英語（SDLのX11ダイアログはCJKフォントを持たない）。 */
        char msg[PAL_USER_DIR_URL_MAX];
        snprintf(msg, sizeof(msg),
                 "Could not open the data folder:\n%s\n\n%s\n\n"
                 "On Linux, xdg-open (xdg-utils) and a file manager are required.",
                 dir, SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ix68k", msg, g_window);
        return -1;
    }
    return 0;
}

const char *pal_get_user_config_dir(void) {
#if defined(__APPLE__) || defined(_WIN32)
    /* macOS・Windowsは設定とデータを分ける慣習が無い。 */
    return pal_get_user_dir();
#else
    /* XDG Base Directory: $XDG_CONFIG_HOME、無ければ $HOME/.config。 */
    static char config_dir[1024];
    if (!config_dir[0]) {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        int n = -1;
        if (xdg && xdg[0] == '/')
            n = snprintf(config_dir, sizeof(config_dir), "%s/ix68k/", xdg);
        else if (home && home[0])
            n = snprintf(config_dir, sizeof(config_dir), "%s/.config/ix68k/", home);
        if (n < 0 || (size_t)n >= sizeof(config_dir) || !SDL_CreateDirectory(config_dir)) {
            config_dir[0] = '\0';
            return pal_get_user_dir();
        }
    }
    return config_dir;
#endif
}

int pal_create_dir(const char *path) {
    return SDL_CreateDirectory(path) ? 0 : -1;
}

uint64_t pal_get_ticks_us(void) {
    static uint64_t freq = 0;
    if (!freq)
        freq = SDL_GetPerformanceFrequency();
    uint64_t c = SDL_GetPerformanceCounter();
    return c / freq * 1000000ULL + c % freq * 1000000ULL / freq;
}

void pal_sleep_us(uint64_t us) {
    SDL_Delay((Uint32)(us / 1000ULL));
}

void pal_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    /* GUIを端末から分離して起動した負荷試験でも、終了前のframeskip／
     * audio statsを確実に回収できるよう即時に出力する。 */
    fflush(stderr);
}
