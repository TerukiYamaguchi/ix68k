#include "app/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "platform/platform.h"

static void set_error(x68k_config_error_t *error, x68k_config_error_code_t code,
                      const char *key, const char *value) {
    if (error->code != X68K_CONFIG_ERROR_NONE)
        return;
    error->code = code;
    strncpy(error->key, key, sizeof(error->key) - 1);
    strncpy(error->value, value ? value : "", sizeof(error->value) - 1);
}

static int parse_bool(const char *v) {
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0;
}

static int parse_cpu_model(const char *v, mc68k_model_t *out) {
    if (strcmp(v, "68000") == 0)
        *out = MC68K_MODEL_68000;
    else if (strcmp(v, "68010") == 0)
        *out = MC68K_MODEL_68010;
    else if (strcmp(v, "68020") == 0)
        *out = MC68K_MODEL_68020;
    else if (strcmp(v, "68ec030") == 0)
        *out = MC68K_MODEL_68EC030;
    else if (strcmp(v, "68030") == 0)
        *out = MC68K_MODEL_68030;
    else if (strcmp(v, "68040") == 0)
        *out = MC68K_MODEL_68040;
    else if (strcmp(v, "68060") == 0)
        *out = MC68K_MODEL_68060;
    else
        return 0;
    return 1;
}

static int parse_fpu(const char *v, x68k_fpu_mode_t *out) {
    if (strcmp(v, "on") == 0)
        *out = X68K_FPU_ON;
    else if (strcmp(v, "off") == 0)
        *out = X68K_FPU_OFF;
    else if (strcmp(v, "68881") == 0)
        *out = X68K_FPU_68881;
    else if (strcmp(v, "68881-secondary") == 0)
        *out = X68K_FPU_68881_SECONDARY;
    else if (strcmp(v, "68882") == 0)
        *out = X68K_FPU_68882;
    else
        return 0;
    return 1;
}
static int parse_timing_profile(const char *v, x68k_timing_profile_t *out) {
    static const char *const names[] = {
        "x68000",
        "xvi",
        "x68030",
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(v, names[i]) == 0) {
            *out = (x68k_timing_profile_t)i;
            return 1;
        }
    }
    return 0;
}

static int parse_accelerator(const char *v, x68k_timing_profile_t *out) {
    if (strcmp(v, "none") == 0)
        *out = X68K_TIMING_PROFILE_NONE;
    else if (strcmp(v, "060turbo") == 0)
        *out = X68K_TIMING_PROFILE_060TURBO;
    else if (strcmp(v, "040turbo") == 0)
        *out = X68K_TIMING_PROFILE_040TURBO;
    else
        return 0;
    return 1;
}

static int parse_bus_timing(const char *v, int *bus_timing_off) {
    if (strcmp(v, "on") == 0)
        *bus_timing_off = 0;
    else if (strcmp(v, "off") == 0)
        *bus_timing_off = 1;
    else
        return 0;
    return 1;
}

static int parse_cpu_clock(const char *v, int *out) {
    char *end;
    long n = strtol(v, &end, 10);
    if (*v == '\0' || *end != '\0' || n < 0 || n > INT_MAX)
        return 0;
    *out = (int)n;
    return 1;
}

/* window_scale / -window-scale: 1、1.5、2（先頭の x は省略可）または fullscreen。
 * 倍率は10倍した整数（10/15/20）で window_zoom_x10 へ、fullscreen は
 * window_fullscreen へ入れる（fullscreen 指定時もウィンドウ表示へ戻した時の倍率は
 * window_zoom_x10 のまま）。 */
static int parse_window_scale(const char *v, x68k_config_t *cfg) {
    static const struct {
        const char *name;
        int zoom_x10;
    } kScales[] = {{"1", X68K_WINDOW_ZOOM_X10_1},
                   {"1.5", X68K_WINDOW_ZOOM_X10_1_5},
                   {"2", X68K_WINDOW_ZOOM_X10_2}};
    if (strcmp(v, "fullscreen") == 0) {
        cfg->window_fullscreen = 1;
        return 1;
    }
    if (v[0] == 'x' || v[0] == 'X')
        v++;
    for (size_t i = 0; i < sizeof(kScales) / sizeof(kScales[0]); i++) {
        if (strcmp(v, kScales[i].name) == 0) {
            cfg->window_zoom_x10 = kScales[i].zoom_x10;
            cfg->window_fullscreen = 0;
            return 1;
        }
    }
    return 0;
}

/* joy1/joy2: 0（自動）または 1..JOYSTICK_PAD_MAX。 */
static int parse_joystick_pad(const char *v, int *out) {
    char *end;
    long n = strtol(v, &end, 10);
    if (*v == '\0' || *end != '\0' || n < 0 || n > JOYSTICK_PAD_MAX)
        return 0;
    *out = (int)n;
    return 1;
}

static int parse_local_memory(const char *v, unsigned *out) {
    if (strcmp(v, "off") == 0 || strcmp(v, "0") == 0) {
        *out = 0;
        return 1;
    }
    if (strcmp(v, "16") == 0 || strcmp(v, "32") == 0 || strcmp(v, "64") == 0 ||
        strcmp(v, "128") == 0) {
        *out = (unsigned)strtoul(v, NULL, 10);
        return 1;
    }
    return 0;
}

static void set_path(char *dst, size_t dstsz, const char *val) {
    strncpy(dst, strcmp(val, "none") == 0 ? "" : val, dstsz - 1);
}

static int config_load_file(const char *path, x68k_config_t *cfg, x68k_config_error_t *error) {
    FILE *f = fopen(path, "r");
    if (!f) {
        /* 探索で見つけた設定は存在確認済み。ここに来るのは -config
         * 指定のファイルが開けないとき。 */
        set_error(error, X68K_CONFIG_ERROR_FILE_OPEN, "-config", path);
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;
        char *eq = strchr(p, '#');
        if (eq)
            *eq = '\0';
        eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = p, *val = eq + 1, *end = key + strlen(key) - 1;
        while (end >= key && (*end == ' ' || *end == '\t'))
            *end-- = '\0';
        while (*val == ' ' || *val == '\t')
            val++;
        end = val + strlen(val) - 1;
        while (end >= val && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
            *end-- = '\0';
        if (strcmp(key, "rom") == 0)
            strncpy(cfg->rom_path, val, sizeof(cfg->rom_path) - 1);
        else if (strcmp(key, "cgrom") == 0)
            strncpy(cfg->cgrom_path, val, sizeof(cfg->cgrom_path) - 1);
        else if (strcmp(key, "sram") == 0)
            strncpy(cfg->sram_path, val, sizeof(cfg->sram_path) - 1);
        else if (strncmp(key, "fd", 2) == 0 && key[2] >= '0' && key[2] <= '3' && key[3] == '\0')
            strncpy(cfg->fd_path[key[2] - '0'], val, sizeof(cfg->fd_path[0]) - 1);
        else if (strncmp(key, "scsi_disk", 9) == 0 && key[9] >= '0' && key[9] <= '6' &&
                 key[10] == '\0')
            strncpy(cfg->scsi_disk_path[key[9] - '0'], val, sizeof(cfg->scsi_disk_path[0]) - 1);
        else if (strncmp(key, "scsi_overlay", 12) == 0 && key[12] >= '0' && key[12] <= '6' &&
                 key[13] == '\0')
            strncpy(cfg->scsi_overlay_path[key[12] - '0'], val,
                    sizeof(cfg->scsi_overlay_path[0]) - 1);
        else if (strcmp(key, "external_fdd") == 0)
            cfg->external_fdd = parse_bool(val);
        else if (strcmp(key, "hostdrv_dir") == 0)
            strncpy(cfg->hostdrv_dir, val, sizeof(cfg->hostdrv_dir) - 1);
        else if (strcmp(key, "keyboard_layout") == 0)
            strncpy(cfg->keyboard_layout, val, sizeof(cfg->keyboard_layout) - 1);
        else if (strcmp(key, "resample_mode") == 0)
            strncpy(cfg->resample_mode, val, sizeof(cfg->resample_mode) - 1);
        else if (strcmp(key, "joystick_select_start") == 0)
            cfg->joystick_select_start = parse_bool(val);
        else if (strcmp(key, "joy1") == 0 || strcmp(key, "joy2") == 0) {
            if (!parse_joystick_pad(val, &cfg->joystick_pad[key[3] - '1']))
                set_error(error, X68K_CONFIG_ERROR_JOYSTICK_PAD, key, val);
        } else if (strcmp(key, "text_auto_probe") == 0)
            cfg->text_auto_probe = parse_bool(val);
        else if (strcmp(key, "window_scale") == 0 && !parse_window_scale(val, cfg))
            set_error(error, X68K_CONFIG_ERROR_WINDOW_SCALE, key, val);
        else if (strcmp(key, "cpu_clock") == 0) {
            if (!parse_cpu_clock(val, &cfg->cpu_clock_mhz))
                set_error(error, X68K_CONFIG_ERROR_CPU_CLOCK, key, val);
            else
                cfg->cpu_clock_specified = true;
        } else if (strcmp(key, "cpu_model") == 0) {
            if (!parse_cpu_model(val, &cfg->cpu_model))
                set_error(error, X68K_CONFIG_ERROR_CPU_MODEL, key, val);
            else
                cfg->cpu_model_specified = true;
        } else if (strcmp(key, "series") == 0 &&
                   !parse_timing_profile(val, &cfg->timing_profile))
            set_error(error, X68K_CONFIG_ERROR_TIMING_PROFILE, key, val);
        else if (strcmp(key, "accelerator") == 0 && !parse_accelerator(val, &cfg->accelerator))
            set_error(error, X68K_CONFIG_ERROR_ACCELERATOR, key, val);
        else if (strcmp(key, "bus_timing") == 0 && !parse_bus_timing(val, &cfg->bus_timing_off))
            set_error(error, X68K_CONFIG_ERROR_BUS_TIMING, key, val);
        else if (strcmp(key, "bus_wait") == 0)
            set_error(error, X68K_CONFIG_ERROR_LEGACY_BUS_WAIT, key, val);
        else if (strcmp(key, "local_memory") == 0 &&
                 !parse_local_memory(val, &cfg->local_memory_mb))
            set_error(error, X68K_CONFIG_ERROR_LOCAL_MEMORY, key, val);
        else if (strcmp(key, "fpu") == 0 && !parse_fpu(val, &cfg->fpu))
            set_error(error, X68K_CONFIG_ERROR_FPU, key, val);
    }
    fclose(f);
    return error->code != X68K_CONFIG_ERROR_NONE;
}

static char s_exe_dir[512];
static char s_user_config_dir[512]; /* 設定の置き場所。Linux以外はs_user_dirと同じ */
static char s_user_dir[512];        /* データ（ROM・ディスク・SRAM）の置き場所 */
static char s_loaded_dir[512];      /* 読んだ設定の相対パスを解決する基準ディレクトリ */
static char s_loaded_path[1024];
static int s_default_created;

/* 初回起動でユーザー領域へ書き出す既定設定（profiles/ix68k.cfg.example と同一内容）。
 * .example を変更したらここも同じ内容に更新する。 */
static const char s_default_config[] =
    "# ix68k の設定ファイル（サンプル兼、初回起動時の自動作成内容）。\n"
    "# ix68k.cfg としてコピーして使う。行頭の # を外すとその項目が有効になる。\n"
    "# 書式: キー = 値（# 以降は行末コメント）。\n"
    "# 相対パスの基準: "
    "実行ファイルの隣やカレントに置いた場合は、この設定ファイルのあるディレクトリ。\n"
    "# ユーザー領域に自動作成された場合は、データフォルダ（File > Open Data Folder で開ける。\n"
    "# Linux では設定ファイルと別の場所）。ROM は rom/、ディスクイメージは disk/ に置く。\n"
    "# パスに none を指定すると「未指定」として扱われる。\n"
    "\n"
    "# ---- ROM / SRAM（必須）----\n"
    "rom = rom/IPLROM.DAT\n"
    "cgrom = rom/CGROM.DAT\n"
    "sram = sram.dat\n"
    "\n"
    "# ---- 機種 / CPU ----\n"
    "# 機種シリーズ（バス wait の表）: x68000（既定。68000/10MHz）| xvi | x68030\n"
    "# series = x68000\n"
    "# X68030 専用の CPU アクセラレータ: none | 060turbo（68060/50MHz）| "
    "040turbo（68040/25・33MHz）\n"
    "# accelerator = none\n"
    "# CPU クロック（MHz）。10 が標準の X68000 相当。\n"
    "cpu_clock = 10\n"
    "# CPU 種別: 68000 | 68010 | 68020 | 68ec030 | 68030 | 68040 | 68060\n"
    "# cpu_model = 68000\n"
    "# FPU: on | off | 68881 | 68881-secondary | 68882\n"
    "# fpu = off\n"
    "# バスタイミング模擬: on | off\n"
    "# bus_timing = on\n"
    "# ローカルメモリ（MB）: off | 16 | 32 | 64 | 128\n"
    "# local_memory = off\n"
    "\n"
    "# ---- フロッピーディスク（fd0〜fd3）----\n"
    "# fd0 = disk/HUMAN302.XDF\n"
    "# fd1 = disk/DATA.XDF\n"
    "# 外付けフロッピードライブを接続する\n"
    "external_fdd = false\n"
    "\n"
    "# ---- SCSI ハードディスク（scsi_disk0〜scsi_disk6）----\n"
    "# scsi_disk0 = disk/SYSTEM_HD.HDS\n"
    "# MO / CD-ROM は特殊値で割り当てる\n"
    "# scsi_disk1 = MODRV\n"
    "# scsi_disk2 = CD\n"
    "# 書き込みを元イメージに反映せず、別ファイルへ逃がすオーバーレイ（scsi_overlay0〜6）\n"
    "# scsi_overlay0 = disk/SYSTEM_HD.overlay\n"
    "\n"
    "# ---- 表示 ----\n"
    "# 画面の大きさ: 1 | 1.5 | 2 | fullscreen（既定 1）。起動オプション -window-scale "
    "でも指定できる。\n"
    "# window_scale = 1\n"
    "\n"
    "# ---- ホスト連携 ----\n"
    "# ホストのフォルダをゲストのドライブとして共有する（HOSTDRV）\n"
    "# hostdrv_dir = /path/to/shared\n"
    "\n"
    "# ---- 入力 / 音 ----\n"
    "# キーボード配列: jis | us\n"
    "keyboard_layout = jis\n"
    "# 音のリサンプル方式: nearest（最速）| linear（既定）| cubic（高品質）\n"
    "resample_mode = linear\n"
    "# ゲームパッドの SELECT / START を、上下 / 左右の同時押しとして X68000 に伝える（既定 "
    "true）。\n"
    "# 方向レバーを普通に倒しただけで SELECT / START が押されたことになる不良パッドでは false "
    "にする。\n"
    "# joystick_select_start = true\n"
    "# X68000 のジョイスティック#1/#2 に使うゲームパッドを、起動ログの\n"
    "# 「Gamepad#N connected: <名前>」の番号 N（1〜8）で指定する。\n"
    "# 0 または未指定なら、#1 は Gamepad#1、#2 は Gamepad#2 を使う。\n"
    "# 繋いだ順と番号が一致しない、1人用ゲームで使いたいパッドが #2 "
    "になる、といった場合に使う。\n"
    "# 設定例: Gamepad#2 を #1 に、Gamepad#1 を #2 にする（2台のパッドの入れ替え）。\n"
    "# joy1 = 2\n"
    "# joy2 = 1\n"
    "\n"
    "# ---- デバッグ ----\n"
    "# HIOCS.X / IOCS.X 常駐時のコンソール出力を、MCP が自動で採取する（既定 true）。\n"
    "# 起動オプション -text-auto-probe <true|false> でも指定できる（起動オプションが優先）。\n"
    "# text_auto_probe = true\n";

const char *x68k_config_loaded_dir(void) {
    return s_loaded_dir[0] ? s_loaded_dir : NULL;
}

const char *x68k_config_loaded_path(void) {
    return s_loaded_path[0] ? s_loaded_path : NULL;
}

int x68k_config_default_created(void) {
    return s_default_created;
}

void x68k_config_set_search_dirs(const char *exe_dir, const char *user_config_dir,
                                 const char *user_dir) {
    snprintf(s_exe_dir, sizeof(s_exe_dir), "%s", exe_dir ? exe_dir : "");
    snprintf(s_user_dir, sizeof(s_user_dir), "%s", user_dir ? user_dir : "");
    /* 設定の置き場所が未指定、またはデータと同じなら、1か所として扱う。 */
    snprintf(s_user_config_dir, sizeof(s_user_config_dir), "%s",
             user_config_dir && user_config_dir[0] ? user_config_dir : s_user_dir);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* 相対パス（ドライブ文字・ルート・UNC を除く）か。空文字列は相対扱いにしない。 */
static int is_relative_path(const char *path) {
    if (path[0] == '\0' || path[0] == '/' || path[0] == '\\')
        return 0;
    return !(path[1] == ':' &&
             ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')));
}

/* 基準ディレクトリが '\\' 区切り（Windowsのユーザー領域等）なら、設定の相対部分の '/' も
 * '\\' にそろえて、表示が混在しないようにする（Windowsはどちらでも開ける）。 */
static void unify_separators_to_dir_style(const char *dir, char *relative) {
    if (!strchr(dir, '\\'))
        return;
    for (char *p = relative; *p; p++) {
        if (*p == '/')
            *p = '\\';
    }
}

static int resolve_path_against(const char *dir, char *path, size_t pathsz, const char *key,
                                x68k_config_error_t *error) {
    if (!is_relative_path(path))
        return 0;
    char joined[1024];
    int n = snprintf(joined, sizeof(joined), "%s%s", dir, path);
    if (n < 0 || (size_t)n >= pathsz) {
        set_error(error, X68K_CONFIG_ERROR_PATH_TOO_LONG, key, path);
        return 1;
    }
    memcpy(path, joined, (size_t)n + 1);
    unify_separators_to_dir_style(dir, path + strlen(dir));
    return 0;
}

/* 設定ファイルのあるディレクトリ基準に、設定のパス項目を解決する。 */
static int resolve_config_paths(const char *dir, x68k_config_t *cfg,
                                x68k_config_error_t *error) {
    char key[24];
    int failed = resolve_path_against(dir, cfg->rom_path, sizeof(cfg->rom_path), "rom", error);
    failed |=
        resolve_path_against(dir, cfg->cgrom_path, sizeof(cfg->cgrom_path), "cgrom", error);
    failed |= resolve_path_against(dir, cfg->sram_path, sizeof(cfg->sram_path), "sram", error);
    failed |= resolve_path_against(dir, cfg->hostdrv_dir, sizeof(cfg->hostdrv_dir),
                                   "hostdrv_dir", error);
    for (int i = 0; i < 4; i++) {
        snprintf(key, sizeof(key), "fd%d", i);
        failed |=
            resolve_path_against(dir, cfg->fd_path[i], sizeof(cfg->fd_path[i]), key, error);
    }
    for (int i = 0; i < 7; i++) {
        snprintf(key, sizeof(key), "scsi_disk%d", i);
        failed |= resolve_path_against(dir, cfg->scsi_disk_path[i],
                                       sizeof(cfg->scsi_disk_path[i]), key, error);
        snprintf(key, sizeof(key), "scsi_overlay%d", i);
        failed |= resolve_path_against(dir, cfg->scsi_overlay_path[i],
                                       sizeof(cfg->scsi_overlay_path[i]), key, error);
    }
    return failed;
}

int x68k_config_find_file(const char *name, char *out, size_t out_size) {
    const char *const dirs[] = {"", s_exe_dir, s_user_config_dir, s_user_dir};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (i > 0 && dirs[i][0] == '\0')
            continue;
        int n = snprintf(out, out_size, "%s%s", dirs[i], name);
        if (n < 0 || (size_t)n >= out_size)
            return 0;
        if (file_exists(out))
            return 1;
    }
    return 0;
}

static int write_default_config(const char *dir, char *path, size_t path_size) {
    if (snprintf(path, path_size, "%six68k.cfg", dir) >= (int)path_size)
        return 0;
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    size_t written = fwrite(s_default_config, 1, sizeof(s_default_config) - 1, f);
    int closed = fclose(f);
    return written == sizeof(s_default_config) - 1 && closed == 0;
}

/* 設定ファイルを読み、相対パスを base_dir 基準に解決する。読んだ場所を記録する。 */
static int load_config_resolving(const char *path, const char *base_dir, x68k_config_t *cfg,
                                 x68k_config_error_t *error) {
    snprintf(s_loaded_dir, sizeof(s_loaded_dir), "%s", base_dir);
    snprintf(s_loaded_path, sizeof(s_loaded_path), "%s", path);
    return config_load_file(path, cfg, error) || resolve_config_paths(base_dir, cfg, error);
}

/* 標準設定ファイルを探して読む。探索順はカレントディレクトリのix68k.cfg →
 * 実行ファイルの隣 →
 * ユーザーの設定領域 → ユーザーのデータ領域。-configで明示指定された場合はこの探索を介さず、
 * 指定パスをそのまま使う。実行ファイルの隣で見つけた設定の相対パスはその隣、ユーザー領域で
 * 見つけた設定の相対パスはユーザーのデータ領域を基準に解決する。
 * どこにも無ければ、allow_create_default のとき既定設定を作る。 */
static int config_load_default(x68k_config_t *cfg, x68k_config_error_t *error,
                               int allow_create_default) {
    s_loaded_dir[0] = '\0';
    s_loaded_path[0] = '\0';
    s_default_created = 0;
    if (file_exists("ix68k.cfg"))
        return config_load_file("ix68k.cfg", cfg, error);
    const struct {
        const char *config_dir;
        const char *base_dir;
    } candidates[] = {
        {s_exe_dir, s_exe_dir},
        {s_user_config_dir, s_user_dir},
        {s_user_dir, s_user_dir},
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        char path[1024];
        if (candidates[i].config_dir[0] == '\0' ||
            snprintf(path, sizeof(path), "%six68k.cfg", candidates[i].config_dir) >=
                (int)sizeof(path) ||
            !file_exists(path))
            continue;
        return load_config_resolving(path, candidates[i].base_dir, cfg, error);
    }
    /* どこにも無い。引数なしの起動（.app / .desktop）なら、ユーザーの設定領域へ既定設定を作る。
     */
    char path[1024];
    if (allow_create_default && s_user_dir[0] &&
        write_default_config(s_user_config_dir, path, sizeof(path))) {
        s_default_created = 1;
        return load_config_resolving(path, s_user_dir, cfg, error);
    }
    return 0;
}

static int config_load_defaults_and_file(int argc, char *argv[], x68k_config_t *cfg,
                                         x68k_config_error_t *error) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->timing_profile = X68K_TIMING_PROFILE_X68000;
    cfg->accelerator = X68K_TIMING_PROFILE_NONE;
    strncpy(cfg->sram_path, "sram.dat", sizeof(cfg->sram_path) - 1);
    strncpy(cfg->keyboard_layout, "jis", sizeof(cfg->keyboard_layout) - 1);
    strncpy(cfg->resample_mode, "linear", sizeof(cfg->resample_mode) - 1);
    cfg->window_zoom_x10 = X68K_WINDOW_ZOOM_X10_1;
    cfg->text_auto_probe = 1;
    cfg->joystick_select_start = 1;
    int no_config = 0;
    int config_count = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-config") == 0 && i + 1 < argc) {
            i++;
            config_count++;
        } else if (strcmp(argv[i], "-no-config") == 0)
            no_config = 1;
    }
    s_loaded_dir[0] = '\0';
    s_loaded_path[0] = '\0';
    s_default_created = 0;
    if (no_config)
        return 0;
    if (config_count == 0)
        return config_load_default(cfg, error, argc == 1);
    /* -config は複数指定でき、指定順に読む。重複するキーは後のファイルが上書きする。 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-config") == 0 && i + 1 < argc) {
            if (config_load_file(argv[++i], cfg, error))
                return 1;
        }
    }
    return 0;
}

static int parse_cli(int argc, char *argv[], x68k_config_t *cfg, x68k_cli_options_t *opt,
                     x68k_config_error_t *error) {
    for (int i = 1; i < argc; i++) {
#define STRING_OPTION(flag, field)                  \
    if (strcmp(argv[i], flag) == 0 && i + 1 < argc) \
    strncpy(field, argv[++i], sizeof(field) - 1)
        STRING_OPTION("-rom", cfg->rom_path);
        else STRING_OPTION("-cgrom", cfg->cgrom_path);
        else STRING_OPTION("-sram", cfg->sram_path);
        else if (strncmp(argv[i], "-fd", 3) == 0 && argv[i][3] >= '0' && argv[i][3] <= '3' &&
                 argv[i][4] == '\0' && i + 1 < argc) {
            int drive = argv[i][3] - '0';
            set_path(cfg->fd_path[drive], sizeof(cfg->fd_path[0]), argv[++i]);
        }
        else if (strlen(argv[i]) == 6 && strncmp(argv[i], "-scsi", 5) == 0 &&
                 argv[i][5] >= '0' && argv[i][5] <= '6' && i + 1 < argc) {
            int id = argv[i][5] - '0';
            set_path(cfg->scsi_disk_path[id], sizeof(cfg->scsi_disk_path[0]), argv[++i]);
        }
        else STRING_OPTION("-hostdrv", cfg->hostdrv_dir);
        else if (strcmp(argv[i], "-ori-illegal") == 0 && i + 1 < argc) cfg->ori_illegal =
            parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-badmovea-log") == 0 && i + 1 < argc) cfg->badmovea_log =
            parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-badaddr-log") == 0 && i + 1 < argc) cfg->badaddr_log =
            parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-badjump-log") == 0 && i + 1 < argc) cfg->badjump_log =
            parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-badori-log") == 0 && i + 1 < argc) cfg->badori_log =
            parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-external-fdd") == 0 && i + 1 < argc) cfg->external_fdd =
            parse_bool(argv[++i]);
        else STRING_OPTION("-boot", cfg->boot_priority);
        else STRING_OPTION("-type", cfg->type_string);
        else if (strcmp(argv[i], "-type-delay") == 0 && i + 1 < argc) cfg->type_delay_sec =
            atoi(argv[++i]);
        else STRING_OPTION("-keyboard-layout", cfg->keyboard_layout);
        else STRING_OPTION("-resample-mode", cfg->resample_mode);
        else if (strcmp(argv[i], "-window-scale") == 0) {
            if (++i >= argc || !parse_window_scale(argv[i], cfg))
                set_error(error, X68K_CONFIG_ERROR_WINDOW_SCALE, "-window-scale",
                          i < argc ? argv[i] : "");
        }
        else if (strcmp(argv[i], "-cpu-clock") == 0) {
            if (++i >= argc || !parse_cpu_clock(argv[i], &cfg->cpu_clock_mhz))
                set_error(error, X68K_CONFIG_ERROR_CPU_CLOCK, "-cpu-clock",
                          i < argc ? argv[i] : "");
            else
                cfg->cpu_clock_specified = true;
        }
        else if (strcmp(argv[i], "-cpu-model") == 0) {
            if (++i >= argc || !parse_cpu_model(argv[i], &cfg->cpu_model))
                set_error(error, X68K_CONFIG_ERROR_CPU_MODEL, "-cpu-model",
                          i < argc ? argv[i] : "");
            else
                cfg->cpu_model_specified = true;
        }
        else if (strcmp(argv[i], "-series") == 0) {
            if (++i >= argc || !parse_timing_profile(argv[i], &cfg->timing_profile))
                set_error(error, X68K_CONFIG_ERROR_TIMING_PROFILE, "-series",
                          i < argc ? argv[i] : "");
        }
        else if (strcmp(argv[i], "-accelerator") == 0) {
            if (++i >= argc || !parse_accelerator(argv[i], &cfg->accelerator))
                set_error(error, X68K_CONFIG_ERROR_ACCELERATOR, "-accelerator",
                          i < argc ? argv[i] : "");
        }
        else if (strcmp(argv[i], "-bus-timing") == 0) {
            if (++i >= argc || !parse_bus_timing(argv[i], &cfg->bus_timing_off))
                set_error(error, X68K_CONFIG_ERROR_BUS_TIMING, "-bus-timing",
                          i < argc ? argv[i] : "");
        }
        else if (strcmp(argv[i], "-bus-wait") == 0) {
            if (i + 1 < argc)
                i++;
            set_error(error, X68K_CONFIG_ERROR_LEGACY_BUS_WAIT, "-bus-wait",
                      i < argc ? argv[i] : "");
        }
        else if (strcmp(argv[i], "-local-memory") == 0 && i + 1 < argc &&
                 !parse_local_memory(argv[++i], &cfg->local_memory_mb))
            set_error(error, X68K_CONFIG_ERROR_LOCAL_MEMORY, "-local-memory", argv[i]);
        else if (strcmp(argv[i], "-fpu") == 0 && i + 1 < argc &&
                 !parse_fpu(argv[++i], &cfg->fpu))
            set_error(error, X68K_CONFIG_ERROR_FPU, "-fpu", argv[i]);
        else if (strcmp(argv[i], "-render-load-us") == 0 && i + 1 < argc) {
            cfg->render_load_us = atoi(argv[++i]);
            if (cfg->render_load_us < 0)
                cfg->render_load_us = 0;
        }
        else if (strcmp(argv[i], "-frame-load-us") == 0 && i + 1 < argc) {
            cfg->frame_load_us = atoi(argv[++i]);
            if (cfg->frame_load_us < 0)
                cfg->frame_load_us = 0;
        }
        else if (strcmp(argv[i], "-run-for-sec") == 0 && i + 1 < argc) {
            cfg->run_for_sec = atoi(argv[++i]);
            if (cfg->run_for_sec < 0)
                cfg->run_for_sec = 0;
        }
        else if (strcmp(argv[i], "-pcring") == 0 && i + 1 < argc) cfg->pcring =
            parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-text-auto-probe") == 0 && i + 1 < argc)
            cfg->text_auto_probe = parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-regring") == 0 && i + 1 < argc) cfg->regring =
            parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-pctrace1024") == 0 && i + 1 < argc) cfg->pctrace1024 =
            parse_bool(argv[++i]);
        else if (strcmp(argv[i], "-raster-sync") == 0 && i + 1 < argc)
            opt->raster_sync_enabled = strcmp(argv[++i], "off") != 0;
        else if (strcmp(argv[i], "-raster-y") == 0 && i + 1 < argc) opt->raster_y_live =
            strcmp(argv[++i], "live") == 0;
        else if (strcmp(argv[i], "-raster-pal") == 0 && i + 1 < argc) opt->raster_pal_live =
            strcmp(argv[++i], "live") == 0;
        else if (strcmp(argv[i], "-mfp-soft-eoi") == 0 && i + 1 < argc)
            opt->mfp_soft_eoi_enabled = strcmp(argv[++i], "on") == 0;
        else if (strcmp(argv[i], "-mfp-gpip-raster") == 0 && i + 1 < argc)
            opt->mfp_gpip_raster_enabled = strcmp(argv[++i], "off") != 0;
        else if (strcmp(argv[i], "-frameskip") == 0 && i + 1 < argc) opt->frameskip_enabled =
            strcmp(argv[++i], "off") != 0;
#undef STRING_OPTION
    }
    return error->code != X68K_CONFIG_ERROR_NONE;
}

const char *x68k_config_usage(void) {
    return "使い方: ix68k [-config <ファイル>] [-rom <IPLROM>] [-cgrom <CGROM>] [-sram "
           "<ファイル>]\n"
           "              [-cpu-model <68000|68010|68020|68ec030|68030|68040|68060>] "
           "[-fpu <off|on|68881|68881-secondary|68882>] "
           "[-cpu-clock <MHz>] [-local-memory <off|0|16|32|64|128>] [-series "
           "<x68000|xvi|x68030>] [-accelerator <none|060turbo|040turbo>] "
           "[-bus-timing <on|off>] "
           "...\n"
           "\n"
           "  -config <ファイル>  設定ファイル（複数指定可、後のものが優先。既定: ix68k.cfg "
           "を、\n                      "
           "カレントディレクトリ、実行ファイルの隣、ユーザー領域の順に探す）\n"
           "  -no-config          設定ファイルを読まない\n"
           "  -rom <IPLROM>       IPLROM のパス（必須。設定ファイルの rom = でも指定可）\n"
           "  -cgrom <CGROM>      CGROM のパス\n"
           "  -series <機種>      機種シリーズ（既定: x68000）。バス wait の表を選ぶ\n"
           "  -accelerator <名前> X68030 専用 CPU アクセラレータ（既定: none）\n"
           "  -window-scale <倍率> 画面の大きさ: 1 | 1.5 | 2 | fullscreen（既定: 1）\n"
           "  -bus-timing <on|off> off でバス wait なし（計測・デバッグ専用。既定: on）\n"
           "  -h, --help          この使い方を表示して終了\n"
           "  -V, --version       バージョンを表示して終了\n"
           "\n"
           "設定例は profiles/ix68k.cfg.example、詳細は docs/ix68k-internals/build.md を参照。";
}

int x68k_config_wants_help(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            return 1;
    }
    return 0;
}

int x68k_config_load_from_args(int argc, char *argv[], x68k_config_t *cfg,
                               x68k_cli_options_t *opt, x68k_config_error_t *error) {
    memset(error, 0, sizeof(*error));
    int config_error = config_load_defaults_and_file(argc, argv, cfg, error);
    return parse_cli(argc, argv, cfg, opt, error) || config_error;
}

int x68k_config_resolve_timing(x68k_config_t *cfg, x68k_config_error_t *error) {
    static const struct {
        mc68k_model_t model;
        int mhz;
    } defaults[] = {{MC68K_MODEL_68000, 10},
                    {MC68K_MODEL_68000, 16},
                    {MC68K_MODEL_68EC030, 25},
                    {MC68K_MODEL_68060, 50},
                    {MC68K_MODEL_68040, 25}};
    if (cfg->timing_profile >= X68K_TIMING_PROFILE_COUNT) {
        set_error(error, X68K_CONFIG_ERROR_TIMING_PROFILE, "series", "");
        return 1;
    }
    if (cfg->accelerator != X68K_TIMING_PROFILE_NONE) {
        /* 060turbo / 040turbo は X68030 専用。 */
        if (cfg->timing_profile != X68K_TIMING_PROFILE_X68030) {
            error->profile = cfg->timing_profile;
            set_error(error, X68K_CONFIG_ERROR_ACCELERATOR_SERIES, "accelerator", "");
            return 1;
        }
        cfg->timing_profile = cfg->accelerator;
    }
    if (cfg->bus_timing_off)
        cfg->timing_profile = X68K_TIMING_PROFILE_NONE;
    if (cfg->timing_profile != X68K_TIMING_PROFILE_NONE) {
        const mc68k_model_t expected = defaults[cfg->timing_profile].model;
        const int mhz = defaults[cfg->timing_profile].mhz;
        if (!cfg->cpu_model_specified)
            cfg->cpu_model = expected;
        if (!cfg->cpu_clock_specified)
            cfg->cpu_clock_mhz = mhz;
        int actual = cfg->cpu_clock_mhz == 0 ? 10 : cfg->cpu_clock_mhz;
        bool model_ok =
            cfg->cpu_model == expected || (cfg->timing_profile == X68K_TIMING_PROFILE_X68030 &&
                                           cfg->cpu_model == MC68K_MODEL_68030);
        /* 040turbo は 25 / 33 MHz のどちらも許可する（既定は 25）。 */
        const bool clock_ok =
            actual == mhz ||
            (cfg->timing_profile == X68K_TIMING_PROFILE_040TURBO && actual == 33);
        if (!model_ok || !clock_ok) {
            error->profile = cfg->timing_profile;
            error->expected_model = expected;
            error->expected_clock_mhz = mhz;
            error->actual_model = cfg->cpu_model;
            error->actual_clock_mhz = actual;
            set_error(error, X68K_CONFIG_ERROR_TIMING_MISMATCH, "series", "");
            return 1;
        }
    }
    if (cfg->cpu_clock_mhz == 0)
        cfg->cpu_clock_mhz = 10;
    if (cfg->cpu_clock_mhz != 10 && cfg->cpu_clock_mhz != 16 && cfg->cpu_clock_mhz != 25 &&
        cfg->cpu_clock_mhz != 33 && cfg->cpu_clock_mhz != 50) {
        char value[sizeof(error->value)];
        snprintf(value, sizeof(value), "%d", cfg->cpu_clock_mhz);
        set_error(error, X68K_CONFIG_ERROR_CPU_CLOCK, "cpu_clock", value);
        return 1;
    }
    return 0;
}

static const char *timing_profile_name(x68k_timing_profile_t profile) {
    static const char *const names[] = {"x68000",   "xvi",      "x68030",
                                        "060turbo", "040turbo", "none"};
    return profile < X68K_TIMING_PROFILE_COUNT ? names[profile] : "?";
}

static const char *cpu_model_name(mc68k_model_t model) {
    switch (model) {
    case MC68K_MODEL_68000:
        return "68000";
    case MC68K_MODEL_68010:
        return "68010";
    case MC68K_MODEL_68020:
        return "68020";
    case MC68K_MODEL_68EC030:
        return "68ec030";
    case MC68K_MODEL_68030:
        return "68030";
    case MC68K_MODEL_68040:
        return "68040";
    case MC68K_MODEL_68060:
        return "68060";
    default:
        return "?";
    }
}

/* 旧 bus_wait の値に対応する移行先 series（none は bus_timing = off）。対応が一意でなければ
 * NULL。 */
static const char *legacy_bus_wait_replacement(const char *value) {
    static const struct {
        const char *old_name;
        const char *new_name;
    } map[] = {
        {"x68000-10", "x68000"},  {"xvi-16", "xvi"}, {"x68030", "x68030"},
        {"060turbo", "060turbo"}, {"none", "none"},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(value, map[i].old_name) == 0)
            return map[i].new_name;
    return NULL;
}

void x68k_config_format_error(const x68k_config_error_t *error, char *buf, size_t size) {
    if (error->code == X68K_CONFIG_ERROR_LEGACY_BUS_WAIT) {
        const char *replacement = legacy_bus_wait_replacement(error->value);
        if (replacement) {
            const bool is_none = strcmp(replacement, "none") == 0;
            const bool is_060 = strcmp(replacement, "060turbo") == 0;
            char target[64];
            if (is_none)
                snprintf(target, sizeof(target), "bus_timing = off");
            else if (is_060)
                snprintf(target, sizeof(target), "series = x68030、accelerator = 060turbo");
            else
                snprintf(target, sizeof(target), "series = %s", replacement);
            snprintf(buf, size, "%s は廃止されました。%s を指定してください%s。", error->key,
                     target, is_none ? "（計測・デバッグ専用）" : "");
        } else
            snprintf(buf, size,
                     "%s = %s は廃止されました。CPU 構成に対応する series "
                     "（x68000、xvi、x68030、accelerator = 060turbo / "
                     "040turbo）を指定してください。",
                     error->key, error->value);
    } else if (error->code == X68K_CONFIG_ERROR_TIMING_MISMATCH) {
        snprintf(buf, size,
                 "series = %s には cpu_model = %s%s、cpu_clock = %d MHz が必要です"
                 "（指定: cpu_model = %s、cpu_clock = %d MHz）。\n"
                 "別機種を使うには対応する series を明示してください。"
                 "bus_timing = off は計測・デバッグ専用です。",
                 timing_profile_name(error->profile), cpu_model_name(error->expected_model),
                 error->profile == X68K_TIMING_PROFILE_X68030 ? "（又は 68030）" : "",
                 error->expected_clock_mhz, cpu_model_name(error->actual_model),
                 error->actual_clock_mhz);
    } else if (error->code == X68K_CONFIG_ERROR_CPU_CLOCK) {
        snprintf(buf, size, "%s の指定値が不正です: %s（10、16、25、33、50 のいずれか）",
                 error->key, error->value);
    } else if (error->code == X68K_CONFIG_ERROR_WINDOW_SCALE) {
        snprintf(buf, size, "%s の指定値が不正です: %s（1、1.5、2、fullscreen のいずれか）",
                 error->key, error->value);
    } else if (error->code == X68K_CONFIG_ERROR_JOYSTICK_PAD) {
        snprintf(buf, size, "%s の指定値が不正です: %s（0〜%d の整数）", error->key,
                 error->value, JOYSTICK_PAD_MAX);
    } else if (error->code == X68K_CONFIG_ERROR_ACCELERATOR) {
        snprintf(buf, size, "%s の指定値が不正です: %s（none、060turbo、040turbo）", error->key,
                 error->value);
    } else if (error->code == X68K_CONFIG_ERROR_ACCELERATOR_SERIES) {
        snprintf(buf, size,
                 "accelerator は X68030 専用です。series = x68030 を指定してください"
                 "（指定: series = %s）。",
                 timing_profile_name(error->profile));
    } else if (error->code == X68K_CONFIG_ERROR_FILE_OPEN) {
        snprintf(buf, size, "設定ファイルを開けません（%s）: %s", error->key, error->value);
    } else if (error->code == X68K_CONFIG_ERROR_PATH_TOO_LONG) {
        snprintf(buf, size, "%s のパスが長すぎます（設定ファイルの場所と連結できません）: %s",
                 error->key, error->value);
    } else if (error->code == X68K_CONFIG_ERROR_BUS_TIMING) {
        snprintf(buf, size, "%s の指定値が不正です: %s（on、off のいずれか）", error->key,
                 error->value);
    } else if (error->code == X68K_CONFIG_ERROR_TIMING_PROFILE) {
        snprintf(buf, size, "%s の指定値が不正です: %s（x68000、xvi、x68030）", error->key,
                 error->value);
    } else {
        snprintf(buf, size, "%s の指定値が不正です: %s", error->key, error->value);
    }
}

audio_resample_mode_t x68k_config_parse_resample_mode(const char *value) {
    if (strcmp(value, "nearest") == 0)
        return AUDIO_RESAMPLE_NEAREST;
    if (strcmp(value, "cubic") == 0)
        return AUDIO_RESAMPLE_CUBIC;
    return AUDIO_RESAMPLE_LINEAR;
}
