#pragma once

#include <stddef.h>

#include "audio/audio_mixer.h"
#include "mc68000/cpu_types.h"
#include "bus/x68k_bus.h"

typedef enum {
    X68K_FPU_OFF,
    X68K_FPU_ON,
    X68K_FPU_68881,
    X68K_FPU_68881_SECONDARY,
    X68K_FPU_68882,
} x68k_fpu_mode_t;

/* 画面の表示倍率（x10表記）。window_scale / -window-scale で選ぶ。 */
#define X68K_WINDOW_ZOOM_X10_1   10
#define X68K_WINDOW_ZOOM_X10_1_5 15
#define X68K_WINDOW_ZOOM_X10_2   20

/* 起動時に確定する設定。永続設定と CLI 専用のデバッグ設定をまとめて保持する。 */
typedef struct {
    char rom_path[512], cgrom_path[512], sram_path[512];
    char fd_path[4][512];
    char scsi_disk_path[7][512];
    char scsi_overlay_path[7][512];
    int ori_illegal, badmovea_log, badaddr_log, badjump_log, badori_log;
    int external_fdd;
    char type_string[256];
    int type_delay_sec;
    char hostdrv_dir[512], keyboard_layout[16], boot_priority[16];
    int pcring, regring, pctrace1024;
    int text_auto_probe; /* 1=MCP が HIOCS.X/IOCS.X の出力採取点を自動で有効化する（既定1） */
    char resample_mode[16];
    int joystick_select_start; /* 1=ゲームパッドのSELECT/STARTを上下/左右同時押しとして出す（既定1）
                                */
    int joystick_pad[2];       /* #1/#2 に使うゲームパッドの列挙番号（1始まり）。0=自動 */
    int cpu_clock_mhz;
    int window_fullscreen;                /* 1=フルスクリーンで起動 */
    int window_zoom_x10;                  /* 画面の表示倍率を10倍した整数（10/15/20）。既定10 */
    unsigned local_memory_mb;             /* 060turbo ローカルメモリ。0=off、16/32/64/128 */
    x68k_timing_profile_t timing_profile; /* series。accelerator 指定時は解決後に置換される */
    x68k_timing_profile_t accelerator;    /* 060TURBO / 040TURBO。未指定は NONE */
    int bus_timing_off;                   /* 1=バス wait なし（計測・デバッグ専用） */
    bool cpu_model_specified;
    bool cpu_clock_specified;
    /* on は68000でCZ-6BP1(MC68881 primary)、030系でFPCP(MC68882)、
     * 68040/68060で内蔵FPUを選ぶ。 */
    x68k_fpu_mode_t fpu;
    mc68k_model_t cpu_model;
    int render_load_us;
    int frame_load_us;
    int run_for_sec;
} x68k_config_t;

/* main.c が所有する実行時フラグのうち、CLI で設定するもの。 */
typedef struct {
    int raster_sync_enabled;
    int raster_y_live;
    int raster_pal_live;
    int mfp_soft_eoi_enabled;
    int mfp_gpip_raster_enabled;
    int frameskip_enabled;
} x68k_cli_options_t;

typedef enum {
    X68K_CONFIG_ERROR_NONE,
    X68K_CONFIG_ERROR_TIMING_PROFILE,
    X68K_CONFIG_ERROR_LEGACY_BUS_WAIT,
    X68K_CONFIG_ERROR_CPU_MODEL,
    X68K_CONFIG_ERROR_CPU_CLOCK,
    X68K_CONFIG_ERROR_TIMING_MISMATCH,
    X68K_CONFIG_ERROR_BUS_TIMING,
    X68K_CONFIG_ERROR_ACCELERATOR,
    X68K_CONFIG_ERROR_ACCELERATOR_SERIES,
    X68K_CONFIG_ERROR_FPU,
    X68K_CONFIG_ERROR_LOCAL_MEMORY,
    X68K_CONFIG_ERROR_PATH_TOO_LONG,
    X68K_CONFIG_ERROR_FILE_OPEN,
    X68K_CONFIG_ERROR_JOYSTICK_PAD,
    X68K_CONFIG_ERROR_WINDOW_SCALE,
} x68k_config_error_code_t;

typedef struct {
    x68k_config_error_code_t code;
    char key[24];
    char value[64];
    x68k_timing_profile_t profile;
    mc68k_model_t expected_model;
    int expected_clock_mhz;
    mc68k_model_t actual_model;
    int actual_clock_mhz;
} x68k_config_error_t;

/* 使い方の文字列（-h / --help と、ROM 未指定エラーで共用）。 */
const char *x68k_config_usage(void);

/* argv に -h / --help があれば 1。 */
int x68k_config_wants_help(int argc, char *argv[]);

/* 標準設定ファイル（ix68k.cfg）の探索先を設定する。探索順はカレントディレクトリ →
 * exe_dir（実行ファイルの隣）→ user_config_dir（ユーザーの設定領域）→ user_dir（ユーザーの
 * データ領域）。-config 指定時は探索しない。各ディレクトリは末尾の区切り文字を含む。
 * NULL または空文字列は無効。user_config_dir が無効なら user_dir と同じ扱い（Linux以外は
 * 設定とデータを分けない）。
 * 実行ファイルの隣で見つけた設定の相対パスはその隣、ユーザー領域で見つけた設定の相対パスは
 * user_dir（データ領域）を基準に解決する（カレントディレクトリは起動方法で変わるため）。 */
void x68k_config_set_search_dirs(const char *exe_dir, const char *user_config_dir,
                                 const char *user_dir);

/* 標準設定ファイルが実行ファイルの隣・ユーザー領域で見つかった（または初回起動で作成した）
 * 場合、その相対パスの解決基準ディレクトリ（末尾の区切り文字を含む）を返す。ROM やディスクの
 * 置き場所になる。カレントディレクトリや -config で読んだ場合、設定ファイルが無い場合は NULL。
 */
const char *x68k_config_loaded_dir(void);

/* 上記の場合に、読んだ（作成した）設定ファイル自体のパス。それ以外は NULL。 */
const char *x68k_config_loaded_path(void);

/* 直前の x68k_config_load_from_args() が、初回起動の既定設定をユーザー領域へ作成したら 1。
 * 作成は、設定ファイルがどこにも無く、引数なしで起動された（.app / .desktop
 * の起動）ときだけ行う。 */
int x68k_config_default_created(void);

/* name を探索先（カレントディレクトリ → 実行ファイルの隣 → ユーザーの設定領域 →
 * データ領域）から探し、 最初に見つかったパスを out に書いて 1 を返す。無い・out
 * に収まらない場合は 0。 keyboard.cfg
 * のように、設定ファイルの中身で相対パスを解決しないファイル用。 */
int x68k_config_find_file(const char *name, char *out, size_t out_size);

/* 組み込み既定値 → 設定ファイル → CLI の順で設定を解決する。 */
/* 成功時0。構文エラーは error に詳細を設定して非0で通知する。 */
int x68k_config_load_from_args(int argc, char *argv[], x68k_config_t *cfg,
                               x68k_cli_options_t *cli_options, x68k_config_error_t *error);
int x68k_config_resolve_timing(x68k_config_t *cfg, x68k_config_error_t *error);
void x68k_config_format_error(const x68k_config_error_t *error, char *buf, size_t buf_size);
audio_resample_mode_t x68k_config_parse_resample_mode(const char *value);
