#include "platform/platform.h"
#include "platform/win_compat.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static int g_quit = 0;
static pal_key_push_fn g_key_push_fn = NULL; /* MFP USART RX FIFO 供給先 */
static pal_key_clear_fn g_key_clear_fn = NULL;
static void *g_key_push_ctx = NULL;
static pal_mouse_position_fn g_mouse_position_fn = NULL; /* SCC ch B 供給先 */
static pal_mouse_button_fn g_mouse_button_fn = NULL;
static void *g_mouse_ctx = NULL;
static int g_host_x = 0, g_host_y = 0; /* MCP mouse_move の相対移動を積算する仮想位置 */

#define X68K_KEY_SHIFT 0x70u /* KeyWitch.X マニュアル準拠のシフトキー物理コード */
#define X68K_KEY_CTRL  0x71u /* KeyWitch.X マニュアル準拠のCTRLキー物理コード */

int pal_init(const char *title, int w, int h) {
    (void)title;
    (void)w;
    (void)h;
    return 0;
}

void pal_quit(void) {
}

void pal_present_frame(const uint32_t *argb, int w, int h) {
    (void)argb;
    (void)w;
    (void)h;
}

void pal_poll_events(void) {
}

int pal_should_quit(void) {
    return g_quit;
}

const char *pal_get_exe_dir(void) {
    return NULL;
}

const char *pal_get_user_dir(void) {
    return NULL;
}

const char *pal_get_user_config_dir(void) {
    return NULL;
}

int pal_open_user_dir(void) {
    return -1;
}

int pal_create_dir(const char *path) {
    (void)path;
    return -1;
}

uint64_t pal_get_ticks_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void pal_sleep_us(uint64_t us) {
    struct timespec ts = {(time_t)(us / 1000000ULL), (long)((us % 1000000ULL) * 1000ULL)};
    nanosleep(&ts, NULL);
}

void pal_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

void pal_show_error(const char *title, const char *msg) {
    fprintf(stderr, "%s: %s\n", title, msg);
    fflush(stderr);
}

void pal_resize(int native_w, int native_h, int display_w, int display_h) {
    (void)native_w;
    (void)native_h;
    (void)display_w;
    (void)display_h;
}

void pal_set_menu_cgrom_ank(const uint8_t *cgrom_ank) {
    (void)cgrom_ank;
}

void pal_key_set_hotkey_config(const pal_hotkey_config_t *config) {
    (void)config;
}

/* ASCII 文字 → (X68k 物理キーコード, SHIFT要否)。platform_sdl3_input.c の
 * ascii_to_x68k_keycode 相当（SDL 非依存版）。マッピングなしは 0 を返す。 */
static uint8_t ascii_to_x68k_keycode(uint8_t ch, int *shift) {
    *shift = 0;
    if (ch >= 'A' && ch <= 'Z') {
        ch = (uint8_t)(ch + 32);
        *shift = 1;
    }
    switch (ch) {
    case 0x1B:
        return 0x01; /* ESC */
    case '1':
        return 0x02;
    case '2':
        return 0x03;
    case '3':
        return 0x04;
    case '4':
        return 0x05;
    case '5':
        return 0x06;
    case '6':
        return 0x07;
    case '7':
        return 0x08;
    case '8':
        return 0x09;
    case '9':
        return 0x0A;
    case '0':
        return 0x0B;
    case '-':
        return 0x0C;
    /* JIS配列 数字段のSHIFT面（platform_sdl3_input.c sdl_to_x68k_keycode 準拠）。 */
    case '!':
        *shift = 1;
        return 0x02;
    case '"':
        *shift = 1;
        return 0x03;
    case '#':
        *shift = 1;
        return 0x04;
    case '$':
        *shift = 1;
        return 0x05;
    case '%':
        *shift = 1;
        return 0x06;
    case '&':
        *shift = 1;
        return 0x07;
    case '\'':
        *shift = 1;
        return 0x08;
    case '(':
        *shift = 1;
        return 0x09;
    case ')':
        *shift = 1;
        return 0x0A;
    case '=':
        *shift = 1;
        return 0x0C;
    case '^':
        return 0x0D;
    case '\\':
        return 0x0E; /* Shift_JISでは¥と同一バイト0x5C */
    case '|':
        *shift = 1;
        return 0x0E;
    case 0x08:
        return 0x0F; /* BS */
    case 0x09:
        return 0x10; /* TAB */
    case 'q':
        return 0x11;
    case 'w':
        return 0x12;
    case 'e':
        return 0x13;
    case 'r':
        return 0x14;
    case 't':
        return 0x15;
    case 'y':
        return 0x16;
    case 'u':
        return 0x17;
    case 'i':
        return 0x18;
    case 'o':
        return 0x19;
    case 'p':
        return 0x1A;
    case '@':
        return 0x1B;
    case '`':
        *shift = 1;
        return 0x1B; /* @`キーのSHIFT面 */
    case '[':
        return 0x1C;
    case '{':
        *shift = 1;
        return 0x1C;
    case 0x0D:
        return 0x1D; /* RETURN */
    case 'a':
        return 0x1E;
    case 's':
        return 0x1F;
    case 'd':
        return 0x20;
    case 'f':
        return 0x21;
    case 'g':
        return 0x22;
    case 'h':
        return 0x23;
    case 'j':
        return 0x24;
    case 'k':
        return 0x25;
    case 'l':
        return 0x26;
    case ';':
        return 0x27;
    case '+':
        *shift = 1;
        return 0x27; /* ;+キーのSHIFT面 */
    case ':':
        return 0x28; /* DIR A: 等で使う ':' を明示的にマッピング */
    case '*':
        *shift = 1;
        return 0x28; /* :*キーのSHIFT面 */
    case ']':
        return 0x29;
    case '}':
        *shift = 1;
        return 0x29;
    case 'z':
        return 0x2A;
    case 'x':
        return 0x2B;
    case 'c':
        return 0x2C;
    case 'v':
        return 0x2D;
    case 'b':
        return 0x2E;
    case 'n':
        return 0x2F;
    case 'm':
        return 0x30;
    case ',':
        return 0x31;
    case '<':
        *shift = 1;
        return 0x31;
    case '.':
        return 0x32;
    case '>':
        *shift = 1;
        return 0x32;
    case '/':
        return 0x33;
    case '?':
        *shift = 1;
        return 0x33;
    case '_':
        /* 実ROM挙動確認済み: kcode 0x34 は unshift面が無出力、shift面が'_'
         * （platform_sdl3_input.c ascii_to_x68k_keycode 準拠）。 */
        *shift = 1;
        return 0x34;
    case ' ':
        return 0x35;
    case 0x7F:
        return 0x37; /* DEL */
    default:
        return 0x00;
    }
}

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

void pal_joystick_set_select_start(int enabled) {
    (void)enabled;
}

void pal_joystick_set_pad(int port, int pad_no) {
    (void)port;
    (void)pad_no;
}

void pal_key_set_us_layout(int is_us) {
    (void)is_us;
}

int pal_nmi_requested(void) {
    return 0;
}

int pal_power_switch_off_requested(void) {
    return 0;
}

void pal_key_set_callbacks(pal_key_push_fn push_fn, pal_key_clear_fn clear_fn, void *ctx) {
    g_key_push_fn = push_fn;
    g_key_clear_fn = clear_fn;
    g_key_push_ctx = ctx;
}

/* ヘッドレス環境には実マウスデバイスが無いが、MCP mouse_move/mouse_click は
 * headlessビルドでも動作する必要がある（キーボードの pal_key_inject_* と同様）。
 * コールバックは登録・保持し、注入関数から直接呼ぶ。 */
void pal_mouse_set_callbacks(pal_mouse_position_fn position_fn, pal_mouse_button_fn button_fn,
                             void *ctx) {
    g_mouse_position_fn = position_fn;
    g_mouse_button_fn = button_fn;
    g_mouse_ctx = ctx;
}

void pal_mouse_inject_motion(int dx, int dy) {
    if (!g_mouse_position_fn)
        return;
    g_host_x += dx;
    g_host_y += dy;
    g_mouse_position_fn(g_mouse_ctx, g_host_x, g_host_y);
}

void pal_mouse_inject_button(int left, int right) {
    if (g_mouse_button_fn)
        g_mouse_button_fn(g_mouse_ctx, left, right);
}

/* ヘッドレス環境にはジョイスティックデバイスが無いが、MCP注入があればそれを返す
 * （CI/Pi Zero等の物理コントローラ無し環境でのテスト用）。 */
static int g_joy_override_active[2] = {0, 0};
static uint8_t g_joy_override_value[2] = {0xFFu, 0xFFu};

uint8_t pal_joystick_read(int index) {
    if (index < 0 || index >= 2)
        return 0xFFu;
    if (g_joy_override_active[index])
        return g_joy_override_value[index];
    return 0xFFu;
}

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

/* ヘッドレス環境には音声出力デバイスが無い。ch3 DMA・
 * コーデック・割り込みは通常どおり動作させ、出力のみ破棄する
 * （既存のPAUSE→NMIテストと同じ切り分け方針）。 */
int pal_audio_init(int sample_rate_hz, int channels) {
    (void)sample_rate_hz;
    (void)channels;
    return 0;
}

int pal_audio_available(void) {
    return 0;
}

int pal_audio_queued_frames(void) {
    return -1;
}

void pal_audio_get_stats(pal_audio_stats_t *out) {
    if (out)
        *out = (pal_audio_stats_t){0};
}

void pal_audio_queue(const int16_t *samples, int frame_count) {
    (void)samples;
    (void)frame_count;
}

void pal_audio_reset(void) {
}

void pal_audio_quit(void) {
}

/* ヘッドレス環境にはマイクデバイスが無い。呼び出し側
 * （adpcm.c経由）はpal_audio_capture_available()==0で無音入力として扱う。 */
int pal_audio_capture_init(int sample_rate_hz, int channels) {
    (void)sample_rate_hz;
    (void)channels;
    return 0;
}

int pal_audio_capture_available(void) {
    return 0;
}

int pal_audio_capture_dequeue(int16_t *out, int max_samples) {
    (void)out;
    (void)max_samples;
    return 0;
}

void pal_audio_capture_quit(void) {
}

void pal_audio_capture_set_active(int active) {
    (void)active;
}

/* ヘッドレス環境にはシリアルポートがない。 */
int pal_serial_init(void) {
    return 0;
}

int pal_serial_available(void) {
    return 0;
}

int pal_serial_read(uint8_t *out, int max) {
    (void)out;
    (void)max;
    return 0;
}

int pal_serial_write(const uint8_t *buf, int n) {
    (void)buf;
    (void)n;
    return 0;
}

void pal_serial_quit(void) {
}

/* ヘッドレス環境には補助ウィンドウを表示するディスプレイが無いため、
 * 生成は常に失敗させる（NULL）。呼び出し側はNULLチェック前提の設計
 * （pal_aux_window_*はwin==NULLで安全にno-opになる）。 */
pal_aux_window_t *pal_aux_window_create(const char *title, int w, int h) {
    (void)title;
    (void)w;
    (void)h;
    return NULL;
}

void pal_aux_window_destroy(pal_aux_window_t *win) {
    (void)win;
}

int pal_aux_window_take_close_request(pal_aux_window_t *win) {
    (void)win;
    return 0;
}

int pal_aux_window_take_click(pal_aux_window_t *win, int *out_x, int *out_y) {
    (void)win;
    (void)out_x;
    (void)out_y;
    return 0;
}

int pal_aux_window_is_pressed(pal_aux_window_t *win, int *out_x, int *out_y) {
    (void)win;
    (void)out_x;
    (void)out_y;
    return 0;
}

int pal_rect_contains(const pal_rect_t *r, int px, int py) {
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

void pal_aux_window_set_key_forward(pal_aux_window_t *win, int enable) {
    (void)win;
    (void)enable;
}

int pal_aux_window_is_kcode_pressed(pal_aux_window_t *win, uint8_t kcode) {
    (void)win;
    (void)kcode;
    return 0;
}

void pal_aux_window_fill_rect(pal_aux_window_t *win, const pal_rect_t *r, uint32_t argb) {
    (void)win;
    (void)r;
    (void)argb;
}

void pal_aux_window_draw_char(pal_aux_window_t *win, int x, int y, char ch,
                              const uint8_t *cgrom_ank, uint32_t argb) {
    (void)win;
    (void)x;
    (void)y;
    (void)ch;
    (void)cgrom_ank;
    (void)argb;
}

void pal_aux_window_draw_text(pal_aux_window_t *win, int x, int y, const char *s,
                              const uint8_t *cgrom_ank, uint32_t argb) {
    (void)win;
    (void)x;
    (void)y;
    (void)s;
    (void)cgrom_ank;
    (void)argb;
}

void pal_aux_window_present(pal_aux_window_t *win) {
    (void)win;
}

pal_aux_hotkey_t pal_aux_window_take_hotkey(void) {
    return PAL_AUX_HOTKEY_NONE;
}

void pal_aux_set_current_window_zoom_x10(int zoom_x10) {
    (void)zoom_x10; /* ヘッドレスにはメニューが無いので無視 */
}

void pal_toggle_fullscreen(void) {
    /* ヘッドレスにはウィンドウが無いので無視 */
}

int pal_is_fullscreen(void) {
    return 0;
}

void pal_aux_window_open_file_dialog(pal_aux_window_t *win, const char *filter_pattern) {
    (void)win;
    (void)filter_pattern;
}

int pal_aux_window_take_file_dialog_result(char *out_path, int out_path_cap) {
    (void)out_path;
    (void)out_path_cap;
    return 0;
}
