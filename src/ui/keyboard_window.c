#include "ui/keyboard_window.h"
#include <stddef.h>

/*
 * キーコードはsrc/platform/platform_sdl3_input.cのsdl_to_x68k_keycode()と同じ体系。
 * 同関数にないテンキー・かな系・CAPS/記号入力/登録/HELP・INS・UNDO は、
 * X68000 のキーコード資料に基づく。
 *
 * ラベルはCGROM ANKフォントがJIS X 0201相当（半角英数のみ、漢字・ひらがな不可、
 * 漢字を表示できないため、実機刻印の日本語（かな/ローマ字/コード入力/記号入力/
 * 登録/全角/ひらがな）はローマ字の略号に置き換えている。矢印キーもANKフォントに
 * 矢印グリフがある保証がないため "<" "^" ">" "v" で代用する。
 */
typedef struct {
    float x, y; /* KB_UNIT単位のグリッド左上座標 */
    float w, h; /* KB_UNIT単位の幅・高さ（RETURN/ENTのみ h=2） */
    const char *label;
    uint8_t kcode;
} kb_key_def_t;

static kb_win_mode_t s_kb_mode = KB_MODE_FULL;

#define KB_UNIT          26
#define KB_GAP           2
#define KB_ORIGIN_X      8
#define KB_ORIGIN_Y      32  /* モード切り替えボタン分の余白を含む */
#define KB_WIN_W         757 /* KB_ORIGIN_X*2 + 28.5u*KB_UNIT（row0が最大幅） */
#define KB_WIN_H         196 /* KB_ORIGIN_Y + 6行*KB_UNIT + 8 */
#define KB_PALETTE_WIN_W 666 /* KB_ORIGIN_X*2 + 25u*KB_UNIT */
#define KB_PALETTE_WIN_H 170 /* KB_ORIGIN_Y + 5行*KB_UNIT + 8 */
#define KB_MODE_BTN_W    90
#define KB_MODE_BTN_H    20

/* ソフトウェアキー押しっぱなし時のオートリピート（物理キーボードの
 * OSキーリピートと挙動を揃える）。初回押下からKB_REPEAT_DELAY_US後にリピート
 * 開始、以後KB_REPEAT_INTERVAL_USごとに押下のみ再送する。 */
#define KB_REPEAT_DELAY_US    500000u
#define KB_REPEAT_INTERVAL_US 33000u

void kb_win_dims(kb_win_mode_t mode, int *out_w, int *out_h) {
    *out_w = (mode == KB_MODE_PALETTE) ? KB_PALETTE_WIN_W : KB_WIN_W;
    *out_h = (mode == KB_MODE_PALETTE) ? KB_PALETTE_WIN_H : KB_WIN_H;
}

pal_aux_window_t *kb_window_create(void) {
    int w, h;
    kb_win_dims(s_kb_mode, &w, &h);
    pal_aux_window_t *win = pal_aux_window_create("Software Keyboard", w, h);
    pal_aux_window_set_key_forward(win, 1);
    return win;
}

kb_win_mode_t kb_window_current_mode(void) {
    return s_kb_mode;
}

static const kb_key_def_t kFullLayout[] = {
    /* row0: BRK/CPY, F1-F10, かな系(KANA/ROMA/CODE), CAPS系(CAPS/KIGO/TOROK/HELP) */
    {0.0f, 0, 1.0f, 1.0f, "BRK", 0x61},
    {1.0f, 0, 1.0f, 1.0f, "CPY", 0x62},
    {2.5f, 0, 1.0f, 1.0f, "F1", 0x63},
    {3.5f, 0, 1.0f, 1.0f, "F2", 0x64},
    {4.5f, 0, 1.0f, 1.0f, "F3", 0x65},
    {5.5f, 0, 1.0f, 1.0f, "F4", 0x66},
    {6.5f, 0, 1.0f, 1.0f, "F5", 0x67},
    {8.0f, 0, 1.0f, 1.0f, "F6", 0x68},
    {9.0f, 0, 1.0f, 1.0f, "F7", 0x69},
    {10.0f, 0, 1.0f, 1.0f, "F8", 0x6A},
    {11.0f, 0, 1.0f, 1.0f, "F9", 0x6B},
    {12.0f, 0, 1.0f, 1.0f, "F10", 0x6C},
    {14.0f, 0, 2.0f, 1.0f, "KANA", 0x5A},
    {16.0f, 0, 2.0f, 1.0f, "ROMA", 0x5B},
    {18.0f, 0, 2.0f, 1.0f, "CODE", 0x5C},
    {20.5f, 0, 2.0f, 1.0f, "CAPS", 0x5D},
    {22.5f, 0, 2.0f, 1.0f, "KIGO", 0x52},
    {24.5f, 0, 2.0f, 1.0f, "TOROK", 0x53},
    {26.5f, 0, 2.0f, 1.0f, "HELP", 0x54},

    /* row1: ESC, 数字段, HOME/INS/DEL, テンキー上段(CLR / * -) */
    {0.0f, 1, 1.0f, 1.0f, "ESC", 0x01},
    {1.0f, 1, 1.0f, 1.0f, "1", 0x02},
    {2.0f, 1, 1.0f, 1.0f, "2", 0x03},
    {3.0f, 1, 1.0f, 1.0f, "3", 0x04},
    {4.0f, 1, 1.0f, 1.0f, "4", 0x05},
    {5.0f, 1, 1.0f, 1.0f, "5", 0x06},
    {6.0f, 1, 1.0f, 1.0f, "6", 0x07},
    {7.0f, 1, 1.0f, 1.0f, "7", 0x08},
    {8.0f, 1, 1.0f, 1.0f, "8", 0x09},
    {9.0f, 1, 1.0f, 1.0f, "9", 0x0A},
    {10.0f, 1, 1.0f, 1.0f, "0", 0x0B},
    {11.0f, 1, 1.0f, 1.0f, "-", 0x0C},
    {12.0f, 1, 1.0f, 1.0f, "^", 0x0D},
    {13.0f, 1, 1.0f, 1.0f, "\\", 0x0E},
    {14.0f, 1, 1.5f, 1.0f, "BS", 0x0F},
    {16.0f, 1, 1.5f, 1.0f, "HOME", 0x36},
    {17.5f, 1, 1.5f, 1.0f, "INS", 0x5E},
    {19.0f, 1, 1.5f, 1.0f, "DEL", 0x37},
    {21.0f, 1, 1.5f, 1.0f, "CLR", 0x3F},
    {22.5f, 1, 1.5f, 1.0f, "/", 0x40},
    {24.0f, 1, 1.5f, 1.0f, "*", 0x41},
    {25.5f, 1, 1.5f, 1.0f, "-", 0x42},

    /* row2: TAB, QWERTY段, RETURN, ROLLUP/ROLLDOWN/UNDO, テンキー7-9 + */
    {0.0f, 2, 1.5f, 1.0f, "TAB", 0x10},
    {1.5f, 2, 1.0f, 1.0f, "Q", 0x11},
    {2.5f, 2, 1.0f, 1.0f, "W", 0x12},
    {3.5f, 2, 1.0f, 1.0f, "E", 0x13},
    {4.5f, 2, 1.0f, 1.0f, "R", 0x14},
    {5.5f, 2, 1.0f, 1.0f, "T", 0x15},
    {6.5f, 2, 1.0f, 1.0f, "Y", 0x16},
    {7.5f, 2, 1.0f, 1.0f, "U", 0x17},
    {8.5f, 2, 1.0f, 1.0f, "I", 0x18},
    {9.5f, 2, 1.0f, 1.0f, "O", 0x19},
    {10.5f, 2, 1.0f, 1.0f, "P", 0x1A},
    {11.5f, 2, 1.0f, 1.0f, "@", 0x1B},
    {12.5f, 2, 1.0f, 1.0f, "[", 0x1C},
    {13.5f, 2, 1.5f, 2.0f, "RET", 0x1D},
    {16.0f, 2, 1.5f, 1.0f, "RU", 0x38},
    {17.5f, 2, 1.5f, 1.0f, "RD", 0x39},
    {19.0f, 2, 1.5f, 1.0f, "UD", 0x3A},
    {21.0f, 2, 1.5f, 1.0f, "7", 0x43},
    {22.5f, 2, 1.5f, 1.0f, "8", 0x44},
    {24.0f, 2, 1.5f, 1.0f, "9", 0x45},
    {25.5f, 2, 1.5f, 1.0f, "+", 0x46},

    /* row3: CTRL, ASDF段, UP矢印, テンキー4-6 = */
    {0.0f, 3, 1.5f, 1.0f, "CTRL", 0x71},
    {1.5f, 3, 1.0f, 1.0f, "A", 0x1E},
    {2.5f, 3, 1.0f, 1.0f, "S", 0x1F},
    {3.5f, 3, 1.0f, 1.0f, "D", 0x20},
    {4.5f, 3, 1.0f, 1.0f, "F", 0x21},
    {5.5f, 3, 1.0f, 1.0f, "G", 0x22},
    {6.5f, 3, 1.0f, 1.0f, "H", 0x23},
    {7.5f, 3, 1.0f, 1.0f, "J", 0x24},
    {8.5f, 3, 1.0f, 1.0f, "K", 0x25},
    {9.5f, 3, 1.0f, 1.0f, "L", 0x26},
    {10.5f, 3, 1.0f, 1.0f, ";", 0x27},
    {11.5f, 3, 1.0f, 1.0f, ":", 0x28},
    {12.5f, 3, 1.0f, 1.0f, "]", 0x29},
    {17.5f, 3, 1.5f, 1.0f, "^", 0x3C},
    {21.0f, 3, 1.5f, 1.0f, "4", 0x47},
    {22.5f, 3, 1.5f, 1.0f, "5", 0x48},
    {24.0f, 3, 1.5f, 1.0f, "6", 0x49},
    {25.5f, 3, 1.5f, 1.0f, "=", 0x4A},

    /* row4: SHIFT(左右), ZXCV段, LEFT/DOWN/RIGHT矢印, テンキー1-3 ENT */
    {0.0f, 4, 2.0f, 1.0f, "SHIFT", 0x70},
    {2.0f, 4, 1.0f, 1.0f, "Z", 0x2A},
    {3.0f, 4, 1.0f, 1.0f, "X", 0x2B},
    {4.0f, 4, 1.0f, 1.0f, "C", 0x2C},
    {5.0f, 4, 1.0f, 1.0f, "V", 0x2D},
    {6.0f, 4, 1.0f, 1.0f, "B", 0x2E},
    {7.0f, 4, 1.0f, 1.0f, "N", 0x2F},
    {8.0f, 4, 1.0f, 1.0f, "M", 0x30},
    {9.0f, 4, 1.0f, 1.0f, ",", 0x31},
    {10.0f, 4, 1.0f, 1.0f, ".", 0x32},
    {11.0f, 4, 1.0f, 1.0f, "/", 0x33},
    {12.0f, 4, 1.0f, 1.0f, "_", 0x34},
    {13.0f, 4, 2.0f, 1.0f, "SHIFT", 0x70},
    {16.0f, 4, 1.5f, 1.0f, "<", 0x3B},
    {17.5f, 4, 1.5f, 1.0f, "v", 0x3E},
    {19.0f, 4, 1.5f, 1.0f, ">", 0x3D},
    {21.0f, 4, 1.5f, 1.0f, "1", 0x4B},
    {22.5f, 4, 1.5f, 1.0f, "2", 0x4C},
    {24.0f, 4, 1.5f, 1.0f, "3", 0x4D},
    {25.5f, 4, 1.5f, 2.0f, "ENT", 0x4E},

    /* row5: HIRA/XF1-5/SPACE/ZEN, OPT.1/OPT.2, テンキー0 , . */
    {0.0f, 5, 1.5f, 1.0f, "HIRA", 0x5F},
    {1.5f, 5, 1.5f, 1.0f, "XF1", 0x55},
    {3.0f, 5, 1.5f, 1.0f, "XF2", 0x56},
    {4.5f, 5, 6.0f, 1.0f, "SPACE", 0x35},
    {10.5f, 5, 1.5f, 1.0f, "XF3", 0x57},
    {12.0f, 5, 1.5f, 1.0f, "XF4", 0x58},
    {13.5f, 5, 1.5f, 1.0f, "XF5", 0x59},
    {15.0f, 5, 1.5f, 1.0f, "ZEN", 0x60},
    {16.0f, 5, 1.5f, 1.0f, "OPT1", 0x72},
    {17.5f, 5, 1.5f, 1.0f, "OPT2", 0x73},
    {21.0f, 5, 1.5f, 1.0f, "0", 0x4F},
    {22.5f, 5, 1.5f, 1.0f, ",", 0x50},
    {24.0f, 5, 1.5f, 1.0f, ".", 0x51},
};
#define KB_FULL_LAYOUT_COUNT ((int)(sizeof(kFullLayout) / sizeof(kFullLayout[0])))

/* パレット表示: ホストキーボードに存在しない・対応が取れないキーだけを表示する。
 * テンキーも、テンキーなしのノートPCではここから入力できる。 */
static const kb_key_def_t kPaletteLayout[] = {
    /* row0: BRK/CPY, かな系, CAPS系, テンキー上段 */
    {0.0f, 0, 1.0f, 1.0f, "BRK", 0x61},
    {1.0f, 0, 1.0f, 1.0f, "CPY", 0x62},
    {3.0f, 0, 2.0f, 1.0f, "KANA", 0x5A},
    {5.0f, 0, 2.0f, 1.0f, "ROMA", 0x5B},
    {7.0f, 0, 2.0f, 1.0f, "CODE", 0x5C},
    {9.5f, 0, 2.0f, 1.0f, "CAPS", 0x5D},
    {11.5f, 0, 2.0f, 1.0f, "KIGO", 0x52},
    {13.5f, 0, 2.0f, 1.0f, "TOROK", 0x53},
    {15.5f, 0, 2.0f, 1.0f, "HELP", 0x54},
    {19.0f, 0, 1.5f, 1.0f, "CLR", 0x3F},
    {20.5f, 0, 1.5f, 1.0f, "/", 0x40},
    {22.0f, 0, 1.5f, 1.0f, "*", 0x41},
    {23.5f, 0, 1.5f, 1.0f, "-", 0x42},

    /* row1: HIRA/XF1-5/ZEN, INS/UNDO, OPT.1/OPT.2, テンキー7-9 + */
    {0.0f, 1, 1.5f, 1.0f, "HIRA", 0x5F},
    {1.5f, 1, 1.5f, 1.0f, "XF1", 0x55},
    {3.0f, 1, 1.5f, 1.0f, "XF2", 0x56},
    {4.5f, 1, 1.5f, 1.0f, "XF3", 0x57},
    {6.0f, 1, 1.5f, 1.0f, "XF4", 0x58},
    {7.5f, 1, 1.5f, 1.0f, "XF5", 0x59},
    {9.0f, 1, 1.5f, 1.0f, "ZEN", 0x60},
    {11.0f, 1, 1.5f, 1.0f, "INS", 0x5E},
    {12.5f, 1, 1.5f, 1.0f, "UD", 0x3A},
    {14.5f, 1, 1.5f, 1.0f, "OPT1", 0x72},
    {16.0f, 1, 1.5f, 1.0f, "OPT2", 0x73},
    {19.0f, 1, 1.5f, 1.0f, "7", 0x43},
    {20.5f, 1, 1.5f, 1.0f, "8", 0x44},
    {22.0f, 1, 1.5f, 1.0f, "9", 0x45},
    {23.5f, 1, 1.5f, 1.0f, "+", 0x46},

    /* row2: テンキー4-6 = */
    {19.0f, 2, 1.5f, 1.0f, "4", 0x47},
    {20.5f, 2, 1.5f, 1.0f, "5", 0x48},
    {22.0f, 2, 1.5f, 1.0f, "6", 0x49},
    {23.5f, 2, 1.5f, 1.0f, "=", 0x4A},

    /* row3: テンキー1-3 ENT */
    {19.0f, 3, 1.5f, 1.0f, "1", 0x4B},
    {20.5f, 3, 1.5f, 1.0f, "2", 0x4C},
    {22.0f, 3, 1.5f, 1.0f, "3", 0x4D},
    {23.5f, 3, 1.5f, 2.0f, "ENT", 0x4E},

    /* row4: テンキー0 , . */
    {19.0f, 4, 1.5f, 1.0f, "0", 0x4F},
    {20.5f, 4, 1.5f, 1.0f, ",", 0x50},
    {22.0f, 4, 1.5f, 1.0f, ".", 0x51},
};
#define KB_PALETTE_LAYOUT_COUNT ((int)(sizeof(kPaletteLayout) / sizeof(kPaletteLayout[0])))

static pal_rect_t kb_key_rect(const kb_key_def_t *k) {
    pal_rect_t r = {
        KB_ORIGIN_X + (int)(k->x * KB_UNIT),
        KB_ORIGIN_Y + (int)(k->y * KB_UNIT),
        (int)(k->w * KB_UNIT) - KB_GAP,
        (int)(k->h * KB_UNIT) - KB_GAP,
    };
    return r;
}

/* CTRL/SHIFT/OPT.1/OPT.2はホスト側で押しっぱなしにできない（クリックの
 * 瞬間だけ押下→離上を送っても組み合わせ入力ができない、実地確認で判明）ため、
 * トグル式にする: 1回目のクリックで押下のみ送りON状態を保持、2回目のクリックで
 * 離上を送りOFFに戻す。それ以外のキーは従来通り押下→即離上。 */
static int kb_kcode_is_toggle(uint8_t kcode) {
    switch (kcode) {
    case 0x70: /* SHIFT(左右とも同じコード) */
    case 0x71: /* CTRL */
    case 0x72: /* OPT.1 */
    case 0x73: /* OPT.2 */
        return 1;
    default:
        return 0;
    }
}

/* XF1-XF5はトグルにせず、マウスで押している間だけ押下を送る（リピートなし）。 */
static int kb_kcode_is_hold_only(uint8_t kcode) {
    return kcode >= 0x55 && kcode <= 0x59;
}

pal_aux_window_t *update_keyboard_window(x68k_bus_t *bus, pal_aux_window_t *win) {
    static uint8_t toggle_on[0x80];
    static uint8_t s_repeat_kcode = 0;    /* 押しっぱなしでリピート中の非トグルキー(0=無し) */
    static uint64_t s_repeat_next_us = 0; /* 次にリピート押下を送るtick(us) */

    if (!win)
        return NULL;
    if (pal_aux_window_take_close_request(win)) {
        /* トグルキー・リピート中キーが押しっぱなしのままウィンドウを閉じると、
         * X68000側でCTRL/SHIFT等が永続的に押下されたままになってしまうため、
         * 閉じる際にON状態を全て解除する。 */
        for (int kc = 0; kc < 0x80; kc++) {
            if (toggle_on[kc]) {
                x68k_bus_mfp_usart_rx_push(bus, (uint8_t)kc | 0x80u);
                toggle_on[kc] = 0;
            }
        }
        if (s_repeat_kcode) {
            x68k_bus_mfp_usart_rx_push(bus, s_repeat_kcode | 0x80u);
            s_repeat_kcode = 0;
        }
        pal_aux_window_destroy(win);
        return NULL;
    }

    const uint8_t *font = x68k_bus_get_cgrom_ank(bus);
    int win_w, win_h;
    kb_win_dims(s_kb_mode, &win_w, &win_h);
    pal_rect_t mode_btn = {win_w - KB_MODE_BTN_W - 8, 6, KB_MODE_BTN_W, KB_MODE_BTN_H};

    int cx, cy;
    if (pal_aux_window_take_click(win, &cx, &cy)) {
        if (pal_rect_contains(&mode_btn, cx, cy)) {
            /* モード切り替え: 補助ウィンドウにリサイズAPIが無いため、
             * destroy→サイズ違いで再createする（トグルキーのON状態は
             * toggle_on[]がstaticなので維持される）。 */
            s_kb_mode = (s_kb_mode == KB_MODE_FULL) ? KB_MODE_PALETTE : KB_MODE_FULL;
            pal_aux_window_destroy(win);
            kb_win_dims(s_kb_mode, &win_w, &win_h);
            win = kb_window_create();
            mode_btn.x = win_w - KB_MODE_BTN_W - 8;
        } else {
            const kb_key_def_t *layout =
                (s_kb_mode == KB_MODE_FULL) ? kFullLayout : kPaletteLayout;
            int count =
                (s_kb_mode == KB_MODE_FULL) ? KB_FULL_LAYOUT_COUNT : KB_PALETTE_LAYOUT_COUNT;
            for (int i = 0; i < count; i++) {
                pal_rect_t r = kb_key_rect(&layout[i]);
                if (pal_rect_contains(&r, cx, cy)) {
                    uint8_t kcode = layout[i].kcode;
                    if (kb_kcode_is_toggle(kcode)) {
                        if (toggle_on[kcode]) {
                            x68k_bus_mfp_usart_rx_push(bus, kcode | 0x80u);
                            toggle_on[kcode] = 0;
                        } else {
                            x68k_bus_mfp_usart_rx_push(bus, kcode);
                            toggle_on[kcode] = 1;
                        }
                    }
                    /* 非トグルキーの押下/離上は下のリピート処理(is_pressed
                     * ベース)が既に送信済みのため、ここでは何もしない。 */
                    break;
                }
            }
        }
    }

    if (!win)
        return NULL; /* モード切り替え直後の再create失敗時のみ */

    /* 押し下げている間はキーをハイライトする（ディスク選択ウィンドウと同じ
     * 発想）。トグルON状態より優先して表示する。 */
    int press_x = -1, press_y = -1;
    int is_pressed = pal_aux_window_is_pressed(win, &press_x, &press_y);

    /* 非トグルキーのオートリピート: 押下開始で即座に押下を送り、押しっぱなしで
     * KB_REPEAT_DELAY_US後からKB_REPEAT_INTERVAL_USごとに押下のみ再送、離すか
     * キーの外へ動かしたら離上を送る（物理キーボードのOSオートリピートと
     * 同じ発想）。トグルキーはON/OFFの往復になってしまうため対象外。 */
    {
        uint8_t hover_kcode = 0;
        if (is_pressed) {
            const kb_key_def_t *layout =
                (s_kb_mode == KB_MODE_FULL) ? kFullLayout : kPaletteLayout;
            int count =
                (s_kb_mode == KB_MODE_FULL) ? KB_FULL_LAYOUT_COUNT : KB_PALETTE_LAYOUT_COUNT;
            for (int i = 0; i < count; i++) {
                pal_rect_t r = kb_key_rect(&layout[i]);
                if (pal_rect_contains(&r, press_x, press_y)) {
                    if (!kb_kcode_is_toggle(layout[i].kcode))
                        hover_kcode = layout[i].kcode;
                    break;
                }
            }
        }
        if (hover_kcode != s_repeat_kcode) {
            if (s_repeat_kcode)
                x68k_bus_mfp_usart_rx_push(bus, s_repeat_kcode | 0x80u);
            if (hover_kcode) {
                x68k_bus_mfp_usart_rx_push(bus, hover_kcode);
                s_repeat_next_us = kb_kcode_is_hold_only(hover_kcode)
                                       ? UINT64_MAX
                                       : pal_get_ticks_us() + KB_REPEAT_DELAY_US;
            }
            s_repeat_kcode = hover_kcode;
        } else if (hover_kcode && !kb_kcode_is_hold_only(hover_kcode)) {
            uint64_t now = pal_get_ticks_us();
            if (now >= s_repeat_next_us) {
                x68k_bus_mfp_usart_rx_push(bus, hover_kcode);
                s_repeat_next_us = now + KB_REPEAT_INTERVAL_US;
            }
        }
    }

    pal_rect_t bg = {0, 0, win_w, win_h};
    pal_aux_window_fill_rect(win, &bg, 0xFF202030u);

    pal_aux_window_fill_rect(win, &mode_btn, 0xFF605020u);
    pal_aux_window_draw_text(win, mode_btn.x + 6, mode_btn.y + 2,
                             s_kb_mode == KB_MODE_FULL ? "PALETTE" : "FULL", font, 0xFFFFFFFFu);

    const kb_key_def_t *layout = (s_kb_mode == KB_MODE_FULL) ? kFullLayout : kPaletteLayout;
    int count = (s_kb_mode == KB_MODE_FULL) ? KB_FULL_LAYOUT_COUNT : KB_PALETTE_LAYOUT_COUNT;
    for (int i = 0; i < count; i++) {
        pal_rect_t r = kb_key_rect(&layout[i]);
        uint8_t kcode = layout[i].kcode;
        int is_on = kb_kcode_is_toggle(kcode) && toggle_on[kcode];
        /* マウスクリック押下中、または対応する物理キーが押されている間
         * ハイライトする（キーボードウィンドウがアクティブな
         * 時に物理キー入力をゲストへ転送する機能と対で、押したキーを
         * 光らせて視覚フィードバックを与える）。 */
        int is_key_pressed = (is_pressed && pal_rect_contains(&r, press_x, press_y)) ||
                             pal_aux_window_is_kcode_pressed(win, kcode);
        uint32_t color = is_key_pressed ? 0xFF80A0F0u : (is_on ? 0xFFE0A030u : 0xFF4060A0u);
        pal_aux_window_fill_rect(win, &r, color);
        pal_aux_window_draw_text(win, r.x + 4, r.y + (r.h - 16) / 2, layout[i].label, font,
                                 0xFFFFFFFFu);
    }

    pal_aux_window_present(win);
    return win;
}
