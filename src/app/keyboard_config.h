#pragma once

/* キーボード専用の設定。ix68k.cfg から分離し、keyboard.cfgで管理する。 */
typedef struct {
    char profile[32];
    char layout[16];
    char disk_window_hotkey[32], software_keyboard_hotkey[32];
    char xf_hotkey[5][32];
    char kigou_hotkey[32], touroku_hotkey[32], break_hotkey[32];
} x68k_keyboard_config_t;

/* pathが無い場合だけlegacy_layoutをレイアウト互換用に使う。 */
void x68k_keyboard_config_load(const char *path, const char *legacy_layout,
                               x68k_keyboard_config_t *cfg);
