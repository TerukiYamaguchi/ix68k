#include "app/keyboard_config.h"

#include <stdio.h>
#include <string.h>

static void copy_value(char *dst, size_t size, const char *src) {
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

static int read_value(FILE *f, char *key, size_t key_size, char *value, size_t value_size) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;
        char *comment = strchr(p, '#');
        if (comment)
            *comment = '\0';
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *end = eq - 1;
        while (end >= p && (*end == ' ' || *end == '\t'))
            *end-- = '\0';
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        end = v + strlen(v);
        while (end > v &&
               (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
            *--end = '\0';
        copy_value(key, key_size, p);
        copy_value(value, value_size, v);
        return 1;
    }
    return 0;
}

static void apply_profile(x68k_keyboard_config_t *cfg, const char *profile) {
    const char *resolved = profile;
    if (strcmp(profile, "auto") == 0) {
#ifdef __APPLE__
        resolved = "macbook_jp";
#else
        resolved = "pc_jp";
#endif
    }
    copy_value(cfg->profile, sizeof(cfg->profile), resolved);
    copy_value(cfg->disk_window_hotkey, sizeof(cfg->disk_window_hotkey), "f12");
    copy_value(cfg->software_keyboard_hotkey, sizeof(cfg->software_keyboard_hotkey),
               "shift+f12");
    for (int i = 0; i < 5; i++)
        copy_value(cfg->xf_hotkey[i], sizeof(cfg->xf_hotkey[i]), "none");
    copy_value(cfg->kigou_hotkey, sizeof(cfg->kigou_hotkey), "none");
    copy_value(cfg->touroku_hotkey, sizeof(cfg->touroku_hotkey), "none");
    copy_value(cfg->break_hotkey, sizeof(cfg->break_hotkey), "none");

    if (strcmp(resolved, "macbook_us") == 0 || strcmp(resolved, "mac_us_keypad") == 0) {
        copy_value(cfg->layout, sizeof(cfg->layout), "us");
    } else if (strcmp(resolved, "macbook_jp") == 0 || strcmp(resolved, "mac_jp_keypad") == 0 ||
               strcmp(resolved, "pc_jp") == 0 || strcmp(resolved, "pc_jp_keypad") == 0) {
        copy_value(cfg->layout, sizeof(cfg->layout), "jis");
    } else if (strcmp(resolved, "pc_us") == 0 || strcmp(resolved, "pc_us_keypad") == 0) {
        copy_value(cfg->layout, sizeof(cfg->layout), "us");
    } else {
        /* 未知profileは安全な自動既定値へ戻す。 */
        apply_profile(cfg, "auto");
        return;
    }

    if (strncmp(resolved, "mac", 3) == 0)
        for (int i = 0; i < 5; i++)
            snprintf(cfg->xf_hotkey[i], sizeof(cfg->xf_hotkey[i]), "gui+f%d", i + 1);
}

static void apply_value(x68k_keyboard_config_t *cfg, const char *key, const char *value) {
    if (strcmp(key, "layout") == 0)
        copy_value(cfg->layout, sizeof(cfg->layout), value);
    else if (strcmp(key, "disk_window_hotkey") == 0)
        copy_value(cfg->disk_window_hotkey, sizeof(cfg->disk_window_hotkey), value);
    else if (strcmp(key, "software_keyboard_hotkey") == 0)
        copy_value(cfg->software_keyboard_hotkey, sizeof(cfg->software_keyboard_hotkey), value);
    else if (strcmp(key, "kigou") == 0)
        copy_value(cfg->kigou_hotkey, sizeof(cfg->kigou_hotkey), value);
    else if (strcmp(key, "touroku") == 0)
        copy_value(cfg->touroku_hotkey, sizeof(cfg->touroku_hotkey), value);
    else if (strcmp(key, "break") == 0)
        copy_value(cfg->break_hotkey, sizeof(cfg->break_hotkey), value);
    else if (strncmp(key, "xf", 2) == 0 && key[2] >= '1' && key[2] <= '5' &&
             strcmp(key + 3, "_hotkey") == 0)
        copy_value(cfg->xf_hotkey[key[2] - '1'], sizeof(cfg->xf_hotkey[0]), value);
}

void x68k_keyboard_config_load(const char *path, const char *legacy_layout,
                               x68k_keyboard_config_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        apply_profile(cfg, "auto");
        if (legacy_layout && strcmp(legacy_layout, "us") == 0)
            copy_value(cfg->layout, sizeof(cfg->layout), "us");
        return;
    }

    char profile[32] = "auto", key[64], value[128];
    while (read_value(f, key, sizeof(key), value, sizeof(value)))
        if (strcmp(key, "profile") == 0)
            copy_value(profile, sizeof(profile), value);
    fclose(f);
    apply_profile(cfg, profile);

    f = fopen(path, "r");
    while (f && read_value(f, key, sizeof(key), value, sizeof(value)))
        apply_value(cfg, key, value);
    if (f)
        fclose(f);
}
