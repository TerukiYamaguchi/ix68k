#include "platform/platform_sdl3_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static pal_key_push_fn g_key_push_fn = NULL; /* MFP USART RX FIFO 供給先 */
static pal_key_clear_fn g_key_clear_fn = NULL;
static void *g_key_push_ctx = NULL;
static pal_mouse_position_fn g_mouse_position_fn = NULL; /* SCC ch B 供給先 */
static pal_mouse_button_fn g_mouse_button_fn = NULL;
static void *g_mouse_ctx = NULL;
/* マウスカーソルはキャプチャ（相対モード）せず、OS標準カーソルをウィンドウ内に
 * ある間だけ隠す（XEiJの「シームレス」モードに準拠）。ウィンドウ内にカーソルが
 * ある間だけSCCへ供給する。ENTER/LEAVEイベントでウィンドウ内外を判定する。
 *
 * ホストの現在位置（ネイティブ解像度換算後）をそのままSCC側へ渡すだけで、
 * こちら側では「X68000側の推定位置」を保持しない（XEiJ Z8530.java準拠）。
 * 差分計算はSCC側がRTS立ち上がり時にIOCSの実カーソル座標を直接参照して
 * 行うため、ウィンドウ境界の出入り等で基準点がズレたまま累積することがない。 */
static int g_mouse_on_screen = 0;
static int g_host_x = 0, g_host_y = 0; /* ホストの現在位置（ネイティブ解像度単位） */
/* ウィンドウはPAR補正でネイティブ解像度より拡大表示される（main.c par_corrected、
 * 例: 2倍/1.5倍）。ホストの座標をネイティブ解像度に換算してから追従させる。 */
static int g_native_w = 1, g_native_h = 1, g_display_w = 1, g_display_h = 1;
/* ホストのゲームパッド。添字+1 が列挙番号（ログの Gamepad#N）。取り外されると NULL に戻り、
 * 番号は他のパッドへ詰め替えない。 */
static SDL_Gamepad *g_controllers[JOYSTICK_PAD_MAX];
/* X68000 のジョイスティック #1/#2 が使う列挙番号（1始まり）。0=自動（#1→1、#2→2）。 */
static int g_joy_pad_no[2] = {0, 0};
/* X68K_JOY_DEBUG=1 で生ジョイスティックのボタン・軸・ハット入力を記録する。 */
static int g_joy_debug = 0;
/* SDL_SetWindowFullscreen(自前F11トグル)・macOS緑丸ボタン(ネイティブ
 * フルスクリーン)どちらで入った場合も生きているウィンドウフラグから
 * 判定する（自前のg_fullscreenだけでは緑丸経由を捕捉できないため）。 */
static int is_fullscreen(void) {
    if (!g_window)
        return 0;
    SDL_WindowFlags flags = SDL_GetWindowFlags(g_window);
    return (flags & SDL_WINDOW_FULLSCREEN) != 0;
}

/* ホストの絶対座標(host_x,host_y)をネイティブ解像度へ換算してSCC側へ渡す。
 * host_x/yはウィンドウ左上原点（pal_corner_insetぶん広げた実ウィンドウ基準・
 * Linux版はpal_menu_bar_heightぶん広げた実ウィンドウ基準）のため、コンテンツ
 * 領域基準に直してからスケーリングする。上端はmacOSのinsetが
 * 無いのでhost_yはそのまま使うが、Linux版メニューバー分だけは差し引く
 * （macOS/Windowsではpal_menu_bar_heightは常に0なので影響しない）。 */
static void sync_mouse_to_host(int host_x, int host_y) {
    if (!g_mouse_position_fn)
        return;
    int content_x = host_x - pal_corner_inset();
    int content_y = host_y - pal_menu_bar_height();
    int target_x =
        g_display_w > 0 ? (int)((long)content_x * g_native_w / g_display_w) : content_x;
    int target_y =
        g_display_h > 0 ? (int)((long)content_y * g_native_h / g_display_h) : content_y;
    /* ウィンドウ境界外から入ってきた場合（ENTER時にホスト座標が枠外を指す）、
     * ネイティブ解像度範囲にクランプする。 */
    if (target_x < 0)
        target_x = 0;
    else if (target_x >= g_native_w)
        target_x = g_native_w - 1;
    if (target_y < 0)
        target_y = 0;
    else if (target_y >= g_native_h)
        target_y = g_native_h - 1;
    g_host_x = target_x;
    g_host_y = target_y;
    g_mouse_position_fn(g_mouse_ctx, g_host_x, g_host_y);
}

/* ウィンドウサイズが変わった直後（F11トグル・緑丸ボタン・手動リサイズ）に
 * 呼ぶ。ウィンドウ境界をまたぐ「移動」が起きていないと SDL は ENTER/LEAVE を
 * 発火しないため（例: 全画面化した瞬間、元々カーソルがあった場所がそのまま
 * 新しいウィンドウ範囲に含まれる/外れる場合）、g_mouse_on_screen が更新されず
 * カーソル表示・追従が壊れたままになる。現在位置から明示的に再判定する。 */
static void refresh_mouse_on_screen_state(void) {
    if (!g_window)
        return;
    float gxf, gyf;
    int wx, wy, ww, wh;
    SDL_GetGlobalMouseState(&gxf, &gyf);
    SDL_GetWindowPosition(g_window, &wx, &wy);
    SDL_GetWindowSize(g_window, &ww, &wh);
    int gx = (int)gxf, gy = (int)gyf;
    int inside = (gx >= wx && gx < wx + ww && gy >= wy && gy < wy + wh);
    if (inside && !g_mouse_on_screen) {
        g_mouse_on_screen = 1;
        SDL_HideCursor();
        sync_mouse_to_host(gx - wx, gy - wy);
    } else if (!inside && g_mouse_on_screen && !is_fullscreen()) {
        g_mouse_on_screen = 0;
        SDL_ShowCursor();
    }
}

void pal_key_set_callbacks(pal_key_push_fn push_fn, pal_key_clear_fn clear_fn, void *ctx) {
    g_key_push_fn = push_fn;
    g_key_clear_fn = clear_fn;
    g_key_push_ctx = ctx;
}

void pal_mouse_set_callbacks(pal_mouse_position_fn position_fn, pal_mouse_button_fn button_fn,
                             void *ctx) {
    g_mouse_position_fn = position_fn;
    g_mouse_button_fn = button_fn;
    g_mouse_ctx = ctx;
}

/* MCPツールの外部インターフェース(相対移動)は変えず、内部で仮想ホスト座標
 * (g_host_x/y)へ加算してから位置コールバックを呼ぶ。 */
void pal_mouse_inject_motion(int dx, int dy) {
    if (!g_mouse_position_fn)
        return;
    g_host_x += dx;
    g_host_y += dy;
    if (g_host_x < 0)
        g_host_x = 0;
    else if (g_host_x >= g_native_w)
        g_host_x = g_native_w - 1;
    if (g_host_y < 0)
        g_host_y = 0;
    else if (g_host_y >= g_native_h)
        g_host_y = g_native_h - 1;
    g_mouse_position_fn(g_mouse_ctx, g_host_x, g_host_y);
}

void pal_mouse_inject_button(int left, int right) {
    if (g_mouse_button_fn)
        g_mouse_button_fn(g_mouse_ctx, left, right);
}

/* device_id（SDL_GetGamepads列挙・SDL_EVENT_GAMEPAD_ADDEDのe.gdevice.whichと
 * 同じくSDL_JoystickID）を空いているg_controllersスロットへ開く。
 * Gamepad非対応デバイス（フライトスティック等）は無視する。
 * 空きが無いときはログに残す（黙って捨てない）。 */
void joystick_try_open(SDL_JoystickID device_id) {
    if (!SDL_IsGamepad(device_id))
        return;
    /* 起動時の列挙とADDEDイベントで同じデバイスが来ても二重に開かない。 */
    for (int i = 0; i < JOYSTICK_PAD_MAX; i++) {
        if (g_controllers[i] &&
            SDL_GetJoystickID(SDL_GetGamepadJoystick(g_controllers[i])) == device_id)
            return;
    }
    for (int i = 0; i < JOYSTICK_PAD_MAX; i++) {
        if (!g_controllers[i]) {
            g_controllers[i] = SDL_OpenGamepad(device_id);
            if (g_controllers[i])
                pal_log("Gamepad#%d connected: %s\n", i + 1,
                        SDL_GetGamepadName(g_controllers[i]));
            else
                pal_log("Gamepad open failed: %s\n", SDL_GetError());
            return;
        }
    }
    pal_log("Gamepad ignored (max %d): %s\n", JOYSTICK_PAD_MAX,
            SDL_GetGamepadNameForID(device_id));
}

/* SDL_EVENT_GAMEPAD_REMOVEDのe.gdevice.whichはinstance ID（SDL3ではADDED/REMOVED
 * 双方ともSDL_JoystickIDで統一されている）のため、開いている各コントローラの
 * joystick instance IDと突き合わせて特定する。 */
void joystick_close_by_instance(SDL_JoystickID instance_id) {
    for (int i = 0; i < JOYSTICK_PAD_MAX; i++) {
        if (g_controllers[i] &&
            SDL_GetJoystickID(SDL_GetGamepadJoystick(g_controllers[i])) == instance_id) {
            pal_log("Gamepad#%d disconnected\n", i + 1);
            SDL_CloseGamepad(g_controllers[i]);
            g_controllers[i] = NULL;
            return;
        }
    }
}

/* MCP注入状態（pal_joystick_inject/release参照）。inject中は実コントローラより優先。 */
static int g_joy_override_active[2] = {0, 0};
static uint8_t g_joy_override_value[2] = {0xFFu, 0xFFu};

void pal_joystick_inject(int index, uint8_t value) {
    if (index < 0 || index >= 2)
        return;
    g_joy_override_active[index] = 1;
    g_joy_override_value[index] = value;
}

void pal_joystick_release(int index) {
    if (index < 0 || index >= 2)
        return;
    g_joy_override_active[index] = 0;
    g_joy_override_value[index] = 0xFFu;
}

/* PPIポートA/Bのビット配置（アクティブロー）。bit0-3が方向、bit5/6がトリガ。 */
#define JOY_BIT_UP    0x01u
#define JOY_BIT_DOWN  0x02u
#define JOY_BIT_LEFT  0x04u
#define JOY_BIT_RIGHT 0x08u
#define JOY_BIT_TRG_A 0x20u
#define JOY_BIT_TRG_B 0x40u

static int g_joy_select_start = 1; /* 0=SELECT/STARTの同時押し合成を行わない */

void pal_joystick_set_pad(int port, int pad_no) {
    if (port < 0 || port >= 2 || pad_no < 0 || pad_no > JOYSTICK_PAD_MAX)
        return;
    g_joy_pad_no[port] = pad_no;
}

void pal_joystick_set_select_start(int enabled) {
    g_joy_select_start = enabled;
}

/* ジョイスティック #0/#1 の状態を X68k PPI ポート A/B 形式へ変換する。 */
uint8_t pal_joystick_read(int index) {
    if (index < 0 || index >= 2)
        return 0xFFu;
    if (g_joy_override_active[index])
        return g_joy_override_value[index];
    int pad_no = g_joy_pad_no[index] ? g_joy_pad_no[index] : index + 1;
    SDL_Gamepad *c = g_controllers[pad_no - 1];
    if (!c)
        return 0xFFu;
    uint8_t v = 0xFFu;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_UP))
        v &= (uint8_t)~JOY_BIT_UP;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
        v &= (uint8_t)~JOY_BIT_DOWN;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
        v &= (uint8_t)~JOY_BIT_LEFT;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        v &= (uint8_t)~JOY_BIT_RIGHT;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_SOUTH))
        v &= (uint8_t)~JOY_BIT_TRG_A;
    if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_EAST))
        v &= (uint8_t)~JOY_BIT_TRG_B;
    /* 4方向+2ボタンのポートでSELECT/STARTを表す慣例: SELECT=上下同時、START(RUN)=左右同時。 */
    if (g_joy_select_start) {
        if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_BACK))
            v &= (uint8_t)~(JOY_BIT_UP | JOY_BIT_DOWN);
        if (SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_START))
            v &= (uint8_t)~(JOY_BIT_LEFT | JOY_BIT_RIGHT);
    }
    return v;
}

#define X68K_KEY_SHIFT 0x70u /* KeyWitch.X マニュアル準拠のシフトキー物理コード */
#define X68K_KEY_CTRL  0x71u /* KeyWitch.X マニュアル準拠のCTRLキー物理コード */
/* US配列';'キーのSHIFT+押下(':'要求)をJIS配列'：＊'キー(0x28)へ橋渡し変換
 * 中かどうかの状態。KEYDOWNで立て、対応するKEYUPで参照する。 */
static int g_semicolon_colon_mode = 0;
/* keyboard.cfg layout キー由来。デフォルト0(JIS、従来動作)。 */
static int g_us_layout = 0;

typedef enum {
    HOTKEY_NONE,
    HOTKEY_DISK_WINDOW,
    HOTKEY_SOFTWARE_KEYBOARD,
    HOTKEY_XF1,
    HOTKEY_XF2,
    HOTKEY_XF3,
    HOTKEY_XF4,
    HOTKEY_XF5,
    HOTKEY_KIGOU,
    HOTKEY_TOUROKU,
    HOTKEY_BREAK,
} hotkey_target_t;

typedef struct {
    SDL_Keycode sym;
    SDL_Keymod mods;
    hotkey_target_t target;
    int held;
} host_hotkey_t;

/* 初期値はmain.cから設定を受け取る前（SDL単体テスト等）にも現行F12操作を保つ。 */
static host_hotkey_t g_hotkeys[] = {
    {SDLK_F12, SDL_KMOD_NONE, HOTKEY_DISK_WINDOW, 0},
    {SDLK_F12, SDL_KMOD_SHIFT, HOTKEY_SOFTWARE_KEYBOARD, 0},
#ifdef __APPLE__
    {SDLK_F1, SDL_KMOD_GUI, HOTKEY_XF1, 0},
    {SDLK_F2, SDL_KMOD_GUI, HOTKEY_XF2, 0},
    {SDLK_F3, SDL_KMOD_GUI, HOTKEY_XF3, 0},
    {SDLK_F4, SDL_KMOD_GUI, HOTKEY_XF4, 0},
    {SDLK_F5, SDL_KMOD_GUI, HOTKEY_XF5, 0},
#else
    {0, SDL_KMOD_NONE, HOTKEY_XF1, 0},
    {0, SDL_KMOD_NONE, HOTKEY_XF2, 0},
    {0, SDL_KMOD_NONE, HOTKEY_XF3, 0},
    {0, SDL_KMOD_NONE, HOTKEY_XF4, 0},
    {0, SDL_KMOD_NONE, HOTKEY_XF5, 0},
#endif
    {0, SDL_KMOD_NONE, HOTKEY_KIGOU, 0},
    {0, SDL_KMOD_NONE, HOTKEY_TOUROKU, 0},
    {0, SDL_KMOD_NONE, HOTKEY_BREAK, 0},
};

static SDL_Keycode fkey_from_number(int n) {
    switch (n) {
    case 1:
        return SDLK_F1;
    case 2:
        return SDLK_F2;
    case 3:
        return SDLK_F3;
    case 4:
        return SDLK_F4;
    case 5:
        return SDLK_F5;
    case 6:
        return SDLK_F6;
    case 7:
        return SDLK_F7;
    case 8:
        return SDLK_F8;
    case 9:
        return SDLK_F9;
    case 10:
        return SDLK_F10;
    case 11:
        return SDLK_F11;
    case 12:
        return SDLK_F12;
    case 13:
        return SDLK_F13;
    case 14:
        return SDLK_F14;
    case 15:
        return SDLK_F15;
    case 16:
        return SDLK_F16;
    case 17:
        return SDLK_F17;
    case 18:
        return SDLK_F18;
    case 19:
        return SDLK_F19;
    case 20:
        return SDLK_F20;
    case 21:
        return SDLK_F21;
    case 22:
        return SDLK_F22;
    case 23:
        return SDLK_F23;
    case 24:
        return SDLK_F24;
    default:
        return 0;
    }
}

static int parse_hotkey(const char *text, host_hotkey_t *out) {
    char value[32];
    size_t len = strlen(text);
    if (len == 0 || len >= sizeof(value))
        return 0;
    for (size_t i = 0; i <= len; i++)
        value[i] = (char)tolower((unsigned char)text[i]);
    if (strcmp(value, "none") == 0) {
        out->sym = 0;
        out->mods = SDL_KMOD_NONE;
        return 1;
    }

    SDL_Keymod mods = SDL_KMOD_NONE;
    char *part = strtok(value, "+");
    char *key_name = NULL;
    while (part) {
        if (strcmp(part, "shift") == 0)
            mods |= SDL_KMOD_SHIFT;
        else if (strcmp(part, "ctrl") == 0)
            mods |= SDL_KMOD_CTRL;
        else if (strcmp(part, "alt") == 0)
            mods |= SDL_KMOD_ALT;
        else if (strcmp(part, "gui") == 0)
            mods |= SDL_KMOD_GUI;
        else if (!key_name)
            key_name = part;
        else
            return 0;
        part = strtok(NULL, "+");
    }
    if (!key_name || key_name[0] != 'f' || key_name[1] == '\0')
        return 0;
    char *end = NULL;
    long n = strtol(key_name + 1, &end, 10);
    if (*end != '\0' || n < 1 || n > 24)
        return 0;
    SDL_Keycode sym = fkey_from_number((int)n);
    if (!sym)
        return 0;
    out->sym = sym;
    out->mods = mods;
    return 1;
}

void pal_key_set_hotkey_config(const pal_hotkey_config_t *config) {
    const char *values[] = {config->disk_window, config->software_keyboard,
                            config->xf[0],       config->xf[1],
                            config->xf[2],       config->xf[3],
                            config->xf[4],       config->kigou,
                            config->touroku,     config->break_key};
    for (size_t i = 0; i < sizeof(g_hotkeys) / sizeof(g_hotkeys[0]); i++) {
        g_hotkeys[i].held = 0;
        if (!values[i] || !parse_hotkey(values[i], &g_hotkeys[i])) {
            pal_log("Invalid keyboard hotkey '%s'; disabled\n", values[i] ? values[i] : "");
            g_hotkeys[i].sym = 0;
            g_hotkeys[i].mods = SDL_KMOD_NONE;
        }
    }
}

static int hotkey_modifiers_match(SDL_Keymod actual, SDL_Keymod required) {
    /* SDL_KMOD_SHIFT等は左右キーのビット和。左Shiftだけのイベント値と
     * SDL_KMOD_SHIFTを直接==比較すると一致しないため、修飾子ごとに有無を比較する。 */
    return !!(actual & SDL_KMOD_SHIFT) == !!(required & SDL_KMOD_SHIFT) &&
           !!(actual & SDL_KMOD_CTRL) == !!(required & SDL_KMOD_CTRL) &&
           !!(actual & SDL_KMOD_ALT) == !!(required & SDL_KMOD_ALT) &&
           !!(actual & SDL_KMOD_GUI) == !!(required & SDL_KMOD_GUI);
}

int sdl_input_hotkey_down(SDL_Keycode sym, SDL_Keymod mod, pal_aux_hotkey_t *out_request) {
    *out_request = PAL_AUX_HOTKEY_NONE;
    for (size_t i = 0; i < sizeof(g_hotkeys) / sizeof(g_hotkeys[0]); i++) {
        host_hotkey_t *hotkey = &g_hotkeys[i];
        if (hotkey->sym != sym || !hotkey_modifiers_match(mod, hotkey->mods))
            continue;
        hotkey->held = 1;
        if (hotkey->target == HOTKEY_DISK_WINDOW)
            *out_request = PAL_AUX_HOTKEY_DISK_WINDOW;
        else if (hotkey->target == HOTKEY_SOFTWARE_KEYBOARD)
            *out_request = PAL_AUX_HOTKEY_KEYBOARD_WINDOW;
        else if (hotkey->target >= HOTKEY_XF1 && hotkey->target <= HOTKEY_XF5)
            sdl_input_send_key((uint8_t)(0x55 + hotkey->target - HOTKEY_XF1), 0);
        else if (hotkey->target == HOTKEY_KIGOU)
            sdl_input_send_key(0x52, 0);
        else if (hotkey->target == HOTKEY_TOUROKU)
            sdl_input_send_key(0x53, 0);
        else
            sdl_input_send_key(0x61, 0);
        return 1;
    }
    return 0;
}

int sdl_input_hotkey_up(SDL_Keycode sym) {
    int consumed = 0;
    for (size_t i = 0; i < sizeof(g_hotkeys) / sizeof(g_hotkeys[0]); i++) {
        host_hotkey_t *hotkey = &g_hotkeys[i];
        if (!hotkey->held || hotkey->sym != sym)
            continue;
        hotkey->held = 0;
        if (hotkey->target >= HOTKEY_XF1 && hotkey->target <= HOTKEY_XF5)
            sdl_input_send_key((uint8_t)(0x55 + hotkey->target - HOTKEY_XF1), 1);
        else if (hotkey->target == HOTKEY_KIGOU)
            sdl_input_send_key(0xD2, 1);
        else if (hotkey->target == HOTKEY_TOUROKU)
            sdl_input_send_key(0xD3, 1);
        else if (hotkey->target == HOTKEY_BREAK)
            sdl_input_send_key(0xE1, 1);
        consumed = 1;
    }
    return consumed;
}

void pal_key_set_us_layout(int is_us) {
    g_us_layout = is_us;
}

/*
 * SDL_Keycode → X68000 キーコード (MFP USART 実プロトコルバイト下位7bit)
 * X68000 キーコードはキーの物理的位置を表す。
 * SDLK_* 定数名はSDL3でも維持されている（値は変わるが本switchは名前で参照）。
 */
static uint8_t sdl_to_x68k_keycode(SDL_Keycode sym) {
    switch (sym) {
    case SDLK_ESCAPE:
        return 0x01;
    case SDLK_1:
        return 0x02;
    case SDLK_2:
        return 0x03;
    case SDLK_3:
        return 0x04;
    case SDLK_4:
        return 0x05;
    case SDLK_5:
        return 0x06;
    case SDLK_6:
        return 0x07;
    case SDLK_7:
        return 0x08;
    case SDLK_8:
        return 0x09;
    case SDLK_9:
        return 0x0A;
    case SDLK_0:
        return 0x0B;
    case SDLK_MINUS:
        return 0x0C;
    /* JIS配列の数字段 SHIFT 面（KeyWitch.X スキャンコード表準拠）。
     * 実キーボードで SHIFT+数字等を押すと SDL がこれらの記号キーコードを
     * 返すため、対応する物理キーへマッピングする。 */
    case SDLK_EXCLAIM:
        return 0x02;
    case SDLK_QUOTEDBL:
        return 0x03;
    case SDLK_HASH:
        return 0x04;
    case SDLK_DOLLAR:
        return 0x05;
    case SDLK_PERCENT:
        return 0x06;
    case SDLK_AMPERSAND:
        return 0x07;
    case SDLK_QUOTE:
        return 0x08;
    case SDLK_LEFTPAREN:
        return 0x09;
    case SDLK_RIGHTPAREN:
        return 0x0A;
    case SDLK_EQUALS:
        return 0x0C;
    case SDLK_CARET:
        return 0x0D;
    case SDLK_BACKSLASH:
        return 0x0E;
    case SDLK_BACKSPACE:
        return 0x0F;
    case SDLK_TAB:
        return 0x10;
    case SDLK_q:
        return 0x11;
    case SDLK_w:
        return 0x12;
    case SDLK_e:
        return 0x13;
    case SDLK_r:
        return 0x14;
    case SDLK_t:
        return 0x15;
    case SDLK_y:
        return 0x16;
    case SDLK_u:
        return 0x17;
    case SDLK_i:
        return 0x18;
    case SDLK_o:
        return 0x19;
    case SDLK_p:
        return 0x1A;
    case SDLK_AT:
        return 0x1B;
    case SDLK_BACKQUOTE:
        return 0x1B;
    case SDLK_LEFTBRACKET:
        return 0x1C;
    case SDLK_RETURN:
        return 0x1D;
    case SDLK_KP_ENTER:
        return 0x4E; /* テンキーEnterは通常Returnとは別の物理キー */
    case SDLK_INSERT:
        return 0x5E;
    case SDLK_UNDO:
        return 0x3A;
    case SDLK_CAPSLOCK:
        return 0x5D;
    case SDLK_COPY:
        return 0x62;
    case SDLK_HELP:
        return 0x54;
    case SDLK_KP_CLEAR:
        return 0x3F;
    case SDLK_KP_DIVIDE:
        return 0x40;
    case SDLK_KP_MULTIPLY:
        return 0x41;
    case SDLK_KP_MINUS:
        return 0x42;
    case SDLK_KP_7:
        return 0x43;
    case SDLK_KP_8:
        return 0x44;
    case SDLK_KP_9:
        return 0x45;
    case SDLK_KP_PLUS:
        return 0x46;
    case SDLK_KP_4:
        return 0x47;
    case SDLK_KP_5:
        return 0x48;
    case SDLK_KP_6:
        return 0x49;
    case SDLK_KP_EQUALS:
        return 0x4A;
    case SDLK_KP_1:
        return 0x4B;
    case SDLK_KP_2:
        return 0x4C;
    case SDLK_KP_3:
        return 0x4D;
    case SDLK_KP_0:
        return 0x4F;
    case SDLK_KP_COMMA:
        return 0x50;
    case SDLK_KP_PERIOD:
        return 0x51;
    case SDLK_a:
        return 0x1E;
    case SDLK_s:
        return 0x1F;
    case SDLK_d:
        return 0x20;
    case SDLK_f:
        return 0x21;
    case SDLK_g:
        return 0x22;
    case SDLK_h:
        return 0x23;
    case SDLK_j:
        return 0x24;
    case SDLK_k:
        return 0x25;
    case SDLK_l:
        return 0x26;
    case SDLK_SEMICOLON:
        return 0x27;
    case SDLK_PLUS:
        return 0x27;
    case SDLK_COLON:
        return 0x28;
    case SDLK_ASTERISK:
        return 0x28;
    case SDLK_RIGHTBRACKET:
        return 0x29;
    case SDLK_z:
        return 0x2A;
    case SDLK_x:
        return 0x2B;
    case SDLK_c:
        return 0x2C;
    case SDLK_v:
        return 0x2D;
    case SDLK_b:
        return 0x2E;
    case SDLK_n:
        return 0x2F;
    case SDLK_m:
        return 0x30;
    case SDLK_COMMA:
        return 0x31;
    case SDLK_PERIOD:
        return 0x32;
    case SDLK_SLASH:
        return 0x33;
    case SDLK_LESS:
        return 0x31;
    case SDLK_GREATER:
        return 0x32;
    case SDLK_QUESTION:
        return 0x33;
    case SDLK_UNDERSCORE:
        return 0x34;
    case SDLK_SPACE:
        return 0x35;
    case SDLK_HOME:
        return 0x36;
    case SDLK_DELETE:
        return 0x37;
    case SDLK_PAGEUP:
        return 0x38;
    case SDLK_PAGEDOWN:
        return 0x39;
    case SDLK_LEFT:
        return 0x3B;
    case SDLK_RIGHT:
        return 0x3D; /* KeyWitch.X: RIGHT=0x3D, UP=0x3C */
    case SDLK_UP:
        return 0x3C;
    case SDLK_DOWN:
        return 0x3E;
    case SDLK_F1:
        return 0x63; /* KeyWitch.X: F1-F10=0x63-0x6C */
    case SDLK_F2:
        return 0x64;
    case SDLK_F3:
        return 0x65;
    case SDLK_F4:
        return 0x66;
    case SDLK_F5:
        return 0x67;
    case SDLK_F6:
        return 0x68;
    case SDLK_F7:
        return 0x69;
    case SDLK_F8:
        return 0x6A;
    case SDLK_F9:
        return 0x6B;
    case SDLK_F10:
        return 0x6C;
    case SDLK_F13:
        return 0x55;
    case SDLK_F14:
        return 0x56;
    case SDLK_F15:
        return 0x57;
    case SDLK_F16:
        return 0x58;
    case SDLK_F17:
        return 0x59;
    /* 修飾キー（KeyWitch.X マニュアル準拠）。ROM 純正ドライバが
     * SHIFT/CTRL 状態を USART プロトコル上のキー押下/離上として解釈するために必要。 */
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        return 0x70;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        return 0x71;
    case SDLK_LALT:
        return 0x72; /* OPT.1 */
    case SDLK_RALT:
        return 0x73; /* OPT.2 */
    default:
        return 0x00;
    }
}

/* ASCII 文字 → (X68k 物理キーコード, SHIFT要否)。マッピングなしは kcode=0 を返す。 */
static uint8_t ascii_to_x68k_keycode(uint8_t ch, int *shift) {
    *shift = 0;
    SDL_Keycode sym;
    switch (ch) {
    case 0x0D:
        sym = SDLK_RETURN;
        break;
    case 0x09:
        sym = SDLK_TAB;
        break;
    case 0x1B:
        sym = SDLK_ESCAPE;
        break;
    case 0x08:
        sym = SDLK_BACKSPACE;
        break;
    case 0x7F:
        sym = SDLK_DELETE;
        break;
    case ' ':
        sym = SDLK_SPACE;
        break;
    default:
        if (ch >= 'A' && ch <= 'Z') {
            sym = (SDL_Keycode)(ch + 32); /* 大文字は小文字キー+SHIFT */
            *shift = 1;
        } else {
            switch (ch) {
            /* JIS配列: 数字段・記号キーの SHIFT 面（KeyWitch.X スキャンコード表
             * 準拠）。US配列とは対応がずれる（例: SHIFT+2='"' で
             * あって'@'ではない）。 */
            case '!':
                sym = SDLK_1;
                *shift = 1;
                break;
            case '"':
                sym = SDLK_2;
                *shift = 1;
                break;
            case '#':
                sym = SDLK_3;
                *shift = 1;
                break;
            case '$':
                sym = SDLK_4;
                *shift = 1;
                break;
            case '%':
                sym = SDLK_5;
                *shift = 1;
                break;
            case '&':
                sym = SDLK_6;
                *shift = 1;
                break;
            case '\'':
                sym = SDLK_7;
                *shift = 1;
                break;
            case '(':
                sym = SDLK_8;
                *shift = 1;
                break;
            case ')':
                sym = SDLK_9;
                *shift = 1;
                break;
            case '=':
                sym = SDLK_MINUS; /* -=キーのSHIFT面 */
                *shift = 1;
                break;
            case '`':
                sym = SDLK_AT; /* @`キーのSHIFT面 */
                *shift = 1;
                break;
            case '{':
                sym = SDLK_LEFTBRACKET;
                *shift = 1;
                break;
            case '}':
                sym = SDLK_RIGHTBRACKET;
                *shift = 1;
                break;
            case '+':
                sym = SDLK_SEMICOLON; /* ;+キーのSHIFT面 */
                *shift = 1;
                break;
            case '*':
                sym = SDLK_COLON; /* :*キーのSHIFT面 */
                *shift = 1;
                break;
            case '<':
                sym = SDLK_COMMA;
                *shift = 1;
                break;
            case '>':
                sym = SDLK_PERIOD;
                *shift = 1;
                break;
            case '?':
                sym = SDLK_SLASH;
                *shift = 1;
                break;
            case '|':
                sym = SDLK_BACKSLASH; /* ¥|キーのSHIFT面 */
                *shift = 1;
                break;
            case '_':
                /* ROM文字テーブル実機確認済み: kcode 0x34 は unshift面が$00
                 * (無出力)、shift面が$5F('_')。KeyWitch.Xマニュアルのキー配置図は
                 * unshiftで'_'と読めるが、実ROMの挙動と食い違う（要SHIFT）。 */
                sym = SDLK_UNDERSCORE;
                *shift = 1;
                break;
            /* SHIFT不要（JIS配列上そのままの unshift 面） */
            case ':':
                sym = SDLK_COLON; /* DIR A: 等で使う ':'
                                     を明示的にマッピング（欠落するとコマンド解析が壊れる） */
                break;
            case '@':
                sym = SDLK_AT;
                break;
            case '[':
                sym = SDLK_LEFTBRACKET;
                break;
            case ']':
                sym = SDLK_RIGHTBRACKET;
                break;
            case '^':
                sym = SDLK_CARET;
                break;
            case '\\':
                sym = SDLK_BACKSLASH; /* Shift_JISでは¥と同一バイト0x5C */
                break;
            default:
                /* 小文字・数字・,-./; 等は SDLK_* 定数値が ASCII と一致するためそのまま引ける
                 */
                sym = (SDL_Keycode)ch;
                break;
            }
        }
        break;
    }
    return sdl_to_x68k_keycode(sym);
}

/* 押下→離上の2バイト（SHIFT要なら前後にSHIFT押下/離上を追加した4バイト）を
 * MFP USART RX FIFO へ送る。実キーボードの1キー入力に相当する。 */
static void mfp_push_key_sequence(uint8_t kcode, int shift, int ctrl) {
    if (!g_key_push_fn)
        return;
    if (shift)
        g_key_push_fn(g_key_push_ctx, X68K_KEY_SHIFT);
    if (ctrl)
        g_key_push_fn(g_key_push_ctx, X68K_KEY_CTRL);
    g_key_push_fn(g_key_push_ctx, kcode);
    g_key_push_fn(g_key_push_ctx, kcode | 0x80u);
    if (ctrl)
        g_key_push_fn(g_key_push_ctx, X68K_KEY_CTRL | 0x80u);
    if (shift)
        g_key_push_fn(g_key_push_ctx, X68K_KEY_SHIFT | 0x80u);
}

void pal_key_inject_ascii(uint8_t ch) {
    uint8_t c = (ch == '\n') ? 0x0D : ch;
    int shift = 0;
    int ctrl = 0;
    /* PASTEの制御文字は実キーボードのCtrl+英字として注入する。BS/TAB/
     * LF/CRは従来どおり編集・改行キーのままにして、テキスト貼り付けとの
     * 互換性を保つ。 */
    if (c >= 0x01 && c <= 0x1A && c != 0x08 && c != 0x09 && c != 0x0A && c != 0x0D) {
        c = (uint8_t)('a' + c - 1);
        ctrl = 1;
    }
    uint8_t kcode = ascii_to_x68k_keycode(c, &shift);
    if (kcode != 0)
        mfp_push_key_sequence(kcode, shift, ctrl);
}

void pal_key_inject_raw(uint32_t val) {
    uint8_t kcode = (uint8_t)((val >> 8) & 0xFFu);
    if (kcode != 0)
        mfp_push_key_sequence(kcode, 0, 0);
}

void pal_key_inject_down(uint32_t val) {
    uint8_t kcode = (uint8_t)((val >> 8) & 0xFFu);
    if (kcode != 0 && g_key_push_fn)
        g_key_push_fn(g_key_push_ctx, kcode);
}

void pal_key_inject_up(uint32_t val) {
    uint8_t kcode = (uint8_t)((val >> 8) & 0xFFu);
    if (kcode != 0 && g_key_push_fn)
        g_key_push_fn(g_key_push_ctx, kcode | 0x80u);
}

void pal_key_clear_queue(void) {
    if (g_key_clear_fn)
        g_key_clear_fn(g_key_push_ctx);
}

uint8_t sdl_input_keycode(SDL_Keycode sym) {
    return sdl_to_x68k_keycode(sym);
}

void sdl_input_send_key(uint8_t kcode, int released) {
    if (kcode != 0 && g_key_push_fn)
        g_key_push_fn(g_key_push_ctx, kcode | (released ? 0x80u : 0));
}

void sdl_input_key_down(SDL_Keycode sym, SDL_Keymod mod) {
    if (sym == SDLK_SEMICOLON && (mod & SDL_KMOD_SHIFT) && g_us_layout) {
        g_semicolon_colon_mode = 1;
        sdl_input_send_key(X68K_KEY_SHIFT, 1);
        sdl_input_send_key(0x28, 0);
        sdl_input_send_key(0x28, 1);
        sdl_input_send_key(X68K_KEY_SHIFT, 0);
        return;
    }
    if (sym == SDLK_SEMICOLON)
        g_semicolon_colon_mode = 0;
    sdl_input_send_key(sdl_to_x68k_keycode(sym), 0);
}

void sdl_input_key_up(SDL_Keycode sym) {
    if (sym == SDLK_SEMICOLON && g_semicolon_colon_mode)
        return;
    sdl_input_send_key(sdl_to_x68k_keycode(sym), 1);
}

void sdl_input_mouse_enter(void) {
    float gxf, gyf;
    int wx, wy;
    g_mouse_on_screen = 1;
    SDL_HideCursor();
    SDL_GetGlobalMouseState(&gxf, &gyf);
    SDL_GetWindowPosition(g_window, &wx, &wy);
    sync_mouse_to_host((int)gxf - wx, (int)gyf - wy);
}

void sdl_input_mouse_leave(void) {
    if (!is_fullscreen()) {
        g_mouse_on_screen = 0;
        SDL_ShowCursor();
    }
}

/* マウス座標(SDL_EVENT_MOUSE_MOTION)はウィンドウ座標(ポイント)なので、換算の
 * 分母もポイント単位のウィンドウサイズから求める。SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
 * のdata1/2はピクセル単位（Retinaでは2倍）で、これを使うと移動量が半分になる。
 * pal_resize()と同じく、insetとメニューバーを除いたコンテンツ領域の大きさにする。 */
void sdl_input_mouse_resized(void) {
    if (!g_window)
        return;
    int ww, wh;
    if (!SDL_GetWindowSize(g_window, &ww, &wh))
        return;
    int display_w = ww - pal_corner_inset() * 2;
    int display_h = wh - pal_corner_inset() - pal_menu_bar_height();
    if (display_w <= 0 || display_h <= 0)
        return;
    g_display_w = display_w;
    g_display_h = display_h;
    refresh_mouse_on_screen_state();
}

void sdl_input_mouse_motion(int x, int y) {
    if (g_mouse_on_screen)
        sync_mouse_to_host(x, y);
}

void sdl_input_mouse_button(void) {
    if (!g_mouse_on_screen)
        return;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(NULL, NULL);
    if (g_mouse_button_fn)
        g_mouse_button_fn(g_mouse_ctx, (buttons & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0,
                          (buttons & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0);
}

void sdl_input_set_dimensions(int native_w, int native_h, int display_w, int display_h) {
    g_native_w = native_w;
    g_native_h = native_h;
    g_display_w = display_w;
    g_display_h = display_h;
}

void sdl_input_gamepad_added(SDL_JoystickID device_id) {
    joystick_try_open(device_id);
}

void sdl_input_gamepad_removed(SDL_JoystickID instance_id) {
    joystick_close_by_instance(instance_id);
}

void sdl_input_debug_joystick_event(const SDL_Event *event) {
    if (!g_joy_debug)
        return;
    switch (event->type) {
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
        pal_log("[joy debug] instance=%d button=%d %s\n", (int)event->jbutton.which,
                event->jbutton.button, event->jbutton.down ? "DOWN" : "UP");
        break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        pal_log("[joy debug] instance=%d axis=%d value=%d\n", (int)event->jaxis.which,
                event->jaxis.axis, event->jaxis.value);
        break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        pal_log("[joy debug] instance=%d hat=%d value=%d\n", (int)event->jhat.which,
                event->jhat.hat, event->jhat.value);
        break;
    }
}

void sdl_input_init_gamepads(void) {
    g_joy_debug = (getenv("X68K_JOY_DEBUG") != NULL);
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        pal_log("SDL_InitSubSystem(GAMEPAD) failed: %s\n", SDL_GetError());
        return;
    }
    const char *base = SDL_GetBasePath();
    if (base) {
        char db_path[1024];
        snprintf(db_path, sizeof(db_path), "%sgamecontrollerdb.txt", base);
        int added = SDL_AddGamepadMappingsFromFile(db_path);
        if (added < 0)
            pal_log("gamecontrollerdb.txt load failed: %s\n", SDL_GetError());
        else
            pal_log("gamecontrollerdb.txt: %d mappings loaded\n", added);
        char user_db_path[1024];
        snprintf(user_db_path, sizeof(user_db_path), "%sgamecontrollerdb_user.txt", base);
        int user_added = SDL_AddGamepadMappingsFromFile(user_db_path);
        if (user_added > 0)
            pal_log("gamecontrollerdb_user.txt: %d mappings loaded\n", user_added);
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids) {
        for (int i = 0; i < n; i++)
            joystick_try_open(ids[i]);
        SDL_free(ids);
    }
    int jn = 0;
    SDL_JoystickID *jids = SDL_GetJoysticks(&jn);
    if (jids) {
        for (int i = 0; i < jn; i++) {
            if (SDL_IsGamepad(jids[i]))
                continue;
            SDL_Joystick *js = SDL_OpenJoystick(jids[i]);
            if (js) {
                char guid_str[64];
                SDL_GUID guid = SDL_GetJoystickGUID(js);
                SDL_GUIDToString(guid, guid_str, sizeof(guid_str));
                pal_log("Joystick not recognized as gamepad: %s (GUID=%s, buttons=%d, axes=%d, "
                        "hats=%d)\n",
                        SDL_GetJoystickName(js), guid_str, SDL_GetNumJoystickButtons(js),
                        SDL_GetNumJoystickAxes(js), SDL_GetNumJoystickHats(js));
                SDL_CloseJoystick(js);
            }
        }
        SDL_free(jids);
    }
}

void sdl_input_quit(void) {
    for (int i = 0; i < JOYSTICK_PAD_MAX; i++) {
        if (g_controllers[i]) {
            SDL_CloseGamepad(g_controllers[i]);
            g_controllers[i] = NULL;
        }
    }
}
