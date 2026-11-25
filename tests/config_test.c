#include "app/config.h"
#include "app/keyboard_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int load_config(int argc, char *argv[], x68k_config_t *cfg, x68k_cli_options_t *opt) {
    x68k_config_error_t error;
    return x68k_config_load_from_args(argc, argv, cfg, opt, &error);
}

/* 2つのファイルが CR を除いて一致するか。開けない・読めない場合は不一致。
 * CRLF の作業ツリーを Linux でビルドしても例ファイルと比較できるようにする。 */
static int files_identical(const char *path_a, const char *path_b) {
    FILE *a = fopen(path_a, "rb");
    FILE *b = fopen(path_b, "rb");
    int same = a && b;
    while (same) {
        int ca, cb;
        while ((ca = fgetc(a)) == '\r') {}
        while ((cb = fgetc(b)) == '\r') {}
        if (ca != cb)
            same = 0;
        else if (ca == EOF)
            break;
    }
    if (a)
        fclose(a);
    if (b)
        fclose(b);
    return same;
}

int main(void) {
    char *argv[] = {"x68000",
                    "-no-config",
                    "-rom",
                    "iplrom.dat",
                    "-hostdrv",
                    "shared",
                    "-fd0",
                    "disk.hdm",
                    "-fd1",
                    "none",
                    "-scsi3",
                    "MO",
                    "-cpu-clock",
                    "25",
                    "-cpu-model",
                    "68ec030",
                    "-raster-sync",
                    "off",
                    "-raster-y",
                    "live",
                    "-raster-pal",
                    "live",
                    "-mfp-soft-eoi",
                    "on",
                    "-mfp-gpip-raster",
                    "off",
                    "-frameskip",
                    "off",
                    "-render-load-us",
                    "20000",
                    "-frame-load-us",
                    "25000",
                    "-run-for-sec",
                    "30",
                    "-pcring",
                    "true",
                    "-badaddr-log",
                    "true"};
    x68k_config_t cfg;
    x68k_cli_options_t opt = {1, 0, 0, 0, 1, 1};
    assert(load_config((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg, &opt) == 0);

    assert(strcmp(cfg.sram_path, "sram.dat") == 0);
    assert(strcmp(cfg.keyboard_layout, "jis") == 0);
    assert(strcmp(cfg.resample_mode, "linear") == 0);
    assert(strcmp(cfg.rom_path, "iplrom.dat") == 0);
    assert(strcmp(cfg.hostdrv_dir, "shared") == 0);
    assert(strcmp(cfg.fd_path[0], "disk.hdm") == 0);
    assert(cfg.fd_path[1][0] == '\0');
    assert(strcmp(cfg.scsi_disk_path[3], "MO") == 0);
    assert(cfg.cpu_clock_mhz == 25);
    assert(cfg.window_zoom_x10 == 10);
    assert(cfg.cpu_model == MC68K_MODEL_68EC030);
    assert(cfg.render_load_us == 20000);
    assert(opt.raster_sync_enabled == 0);
    assert(opt.raster_y_live == 1);
    assert(opt.raster_pal_live == 1);
    assert(opt.mfp_soft_eoi_enabled == 1);
    assert(opt.mfp_gpip_raster_enabled == 0);
    assert(opt.frameskip_enabled == 0);
    assert(cfg.run_for_sec == 30);
    assert(cfg.frame_load_us == 25000);
    assert(cfg.pcring == 1);
    assert(cfg.badaddr_log == 1);
    assert(x68k_config_parse_resample_mode("nearest") == AUDIO_RESAMPLE_NEAREST);
    assert(x68k_config_parse_resample_mode("unknown") == AUDIO_RESAMPLE_LINEAR);

    FILE *overlay_file = fopen("scsi_overlay_config_test.cfg", "w");
    assert(overlay_file);
    fputs("scsi_disk0 = disk/base.hds\nscsi_overlay0 = disk/base.cow\n", overlay_file);
    fclose(overlay_file);
    char *overlay_argv[] = {"x68000", "-config", "scsi_overlay_config_test.cfg"};
    assert(load_config(3, overlay_argv, &cfg, &opt) == 0);
    assert(strcmp(cfg.scsi_disk_path[0], "disk/base.hds") == 0);
    assert(strcmp(cfg.scsi_overlay_path[0], "disk/base.cow") == 0);
    assert(remove("scsi_overlay_config_test.cfg") == 0);

    /* joystick_select_start: 既定は有効。.cfg で無効化できる。 */
    assert(cfg.joystick_select_start == 1);
    FILE *joy_ss_file = fopen("joystick_select_start_config_test.cfg", "w");
    assert(joy_ss_file);
    fputs("joystick_select_start = false\n", joy_ss_file);
    fclose(joy_ss_file);
    char *joy_ss_off_argv[] = {"x68000", "-config", "joystick_select_start_config_test.cfg"};
    assert(load_config(3, joy_ss_off_argv, &cfg, &opt) == 0);
    assert(cfg.joystick_select_start == 0);
    assert(remove("joystick_select_start_config_test.cfg") == 0);

    /* window_scale: 既定は x1。.cfg と -window-scale で 1 / 1.5 / 2 を選べ、CLI が優先。
     * 不正値は設定エラー。 */
    FILE *scale_file = fopen("window_scale_config_test.cfg", "w");
    assert(scale_file);
    fputs("window_scale = 1.5\n", scale_file);
    fclose(scale_file);
    char *scale_cfg_argv[] = {"x68000", "-config", "window_scale_config_test.cfg"};
    assert(load_config(3, scale_cfg_argv, &cfg, &opt) == 0);
    assert(cfg.window_zoom_x10 == 15);
    char *scale_cli_argv[] = {"x68000", "-config", "window_scale_config_test.cfg",
                              "-window-scale", "x2"};
    assert(load_config(5, scale_cli_argv, &cfg, &opt) == 0);
    assert(cfg.window_zoom_x10 == 20);
    char *scale_bad_argv[] = {"x68000", "-no-config", "-window-scale", "3"};
    assert(load_config(4, scale_bad_argv, &cfg, &opt) != 0);
    assert(cfg.window_fullscreen == 0);
    char *scale_fs_argv[] = {"x68000", "-no-config", "-window-scale", "fullscreen"};
    assert(load_config(4, scale_fs_argv, &cfg, &opt) == 0);
    assert(cfg.window_fullscreen == 1 && cfg.window_zoom_x10 == 10);
    assert(remove("window_scale_config_test.cfg") == 0);

    /* joy1/joy2: 既定は0（自動）。.cfg で列挙番号を指定でき、範囲外・非数は設定エラー。 */
    assert(cfg.joystick_pad[0] == 0 && cfg.joystick_pad[1] == 0);
    FILE *joy_pad_file = fopen("joystick_pad_config_test.cfg", "w");
    assert(joy_pad_file);
    fputs("joy1 = 2\njoy2 = 1\n", joy_pad_file);
    fclose(joy_pad_file);
    char *joy_pad_argv[] = {"x68000", "-config", "joystick_pad_config_test.cfg"};
    assert(load_config(3, joy_pad_argv, &cfg, &opt) == 0);
    assert(cfg.joystick_pad[0] == 2 && cfg.joystick_pad[1] == 1);
    assert(remove("joystick_pad_config_test.cfg") == 0);

    /* text_auto_probe: 既定は有効。.cfg で無効化でき、CLI が .cfg より優先される。 */
    assert(cfg.text_auto_probe == 1);
    FILE *auto_probe_file = fopen("text_auto_probe_config_test.cfg", "w");
    assert(auto_probe_file);
    fputs("text_auto_probe = false\n", auto_probe_file);
    fclose(auto_probe_file);
    char *auto_probe_off_argv[] = {"x68000", "-config", "text_auto_probe_config_test.cfg"};
    assert(load_config(3, auto_probe_off_argv, &cfg, &opt) == 0);
    assert(cfg.text_auto_probe == 0);
    char *auto_probe_cli_on_argv[] = {"x68000", "-config", "text_auto_probe_config_test.cfg",
                                      "-text-auto-probe", "true"};
    assert(load_config(5, auto_probe_cli_on_argv, &cfg, &opt) == 0);
    assert(cfg.text_auto_probe == 1);
    char *auto_probe_cli_off_argv[] = {"x68000", "-text-auto-probe", "false"};
    assert(load_config(3, auto_probe_cli_off_argv, &cfg, &opt) == 0);
    assert(cfg.text_auto_probe == 0);
    assert(remove("text_auto_probe_config_test.cfg") == 0);

    FILE *keyboard_file = fopen("keyboard_config_test.cfg", "w");
    assert(keyboard_file);
    fputs("xf1_hotkey = ctrl+f1\nkigou = ctrl+f6\nprofile = pc_us_keypad\n", keyboard_file);
    fclose(keyboard_file);
    x68k_keyboard_config_t keyboard_cfg;
    x68k_keyboard_config_load("keyboard_config_test.cfg", "jis", &keyboard_cfg);
    assert(strcmp(keyboard_cfg.profile, "pc_us_keypad") == 0);
    assert(strcmp(keyboard_cfg.layout, "us") == 0);
    assert(strcmp(keyboard_cfg.disk_window_hotkey, "f12") == 0);
    assert(strcmp(keyboard_cfg.xf_hotkey[0], "ctrl+f1") == 0);
    assert(strcmp(keyboard_cfg.xf_hotkey[1], "none") == 0);
    assert(strcmp(keyboard_cfg.kigou_hotkey, "ctrl+f6") == 0);
    assert(remove("keyboard_config_test.cfg") == 0);

    char *legacy_argv[] = {"x68000", "-no-config", "-hostfs", "legacy-shared"};
    assert(load_config((int)(sizeof(legacy_argv) / sizeof(legacy_argv[0])), legacy_argv, &cfg,
                       &opt) == 0);
    assert(cfg.hostdrv_dir[0] == '\0');
    assert(cfg.badaddr_log == 0);

    char *badaddr_off_argv[] = {"x68000", "-no-config", "-badaddr-log", "false"};
    assert(load_config((int)(sizeof(badaddr_off_argv) / sizeof(badaddr_off_argv[0])),
                       badaddr_off_argv, &cfg, &opt) == 0);
    assert(cfg.badaddr_log == 0);

    /* FPU 統合設定: 既定は off、68000 の on は primary 68881、CLI が設定ファイルを上書きする。
     */
    char *fpu_default_argv[] = {"x68000", "-no-config"};
    assert(load_config(2, fpu_default_argv, &cfg, &opt) == 0);
    assert(cfg.fpu == X68K_FPU_OFF);
    assert(cfg.local_memory_mb == 0);
    assert(cfg.timing_profile == X68K_TIMING_PROFILE_X68000);
    /* -config に存在しないファイルを指定したら、ファイルを開けないエラーにする。 */
    {
        char *missing_argv[] = {"x68000", "-config", "missing_config_test.cfg"};
        x68k_config_error_t missing_error;
        assert(x68k_config_load_from_args(3, missing_argv, &cfg, &opt, &missing_error) != 0);
        assert(missing_error.code == X68K_CONFIG_ERROR_FILE_OPEN);
        char missing_msg[256];
        x68k_config_format_error(&missing_error, missing_msg, sizeof(missing_msg));
        assert(strstr(missing_msg, "missing_config_test.cfg") != NULL);
    }
    x68k_config_error_t timing_error = {0};
    assert(x68k_config_resolve_timing(&cfg, &timing_error) == 0);
    assert(cfg.cpu_model == MC68K_MODEL_68000 && cfg.cpu_clock_mhz == 10);
    static const char *const timing_names[] = {"x68000", "xvi", "x68030"};
    for (unsigned i = 0; i < sizeof(timing_names) / sizeof(timing_names[0]); i++) {
        char *timing_argv[] = {"x68000", "-no-config", "-series", (char *)timing_names[i]};
        assert(load_config(4, timing_argv, &cfg, &opt) == 0);
        assert(cfg.timing_profile == (x68k_timing_profile_t)i);
    }
    char *xvi_argv[] = {"x68000", "-no-config", "-series", "xvi"};
    assert(load_config(4, xvi_argv, &cfg, &opt) == 0);
    memset(&timing_error, 0, sizeof(timing_error));
    assert(x68k_config_resolve_timing(&cfg, &timing_error) == 0);
    assert(cfg.cpu_model == MC68K_MODEL_68000 && cfg.cpu_clock_mhz == 16);
    char *bus_off_argv[] = {"x68000", "-no-config", "-series", "xvi", "-bus-timing", "off"};
    assert(load_config(6, bus_off_argv, &cfg, &opt) == 0);
    assert(x68k_config_resolve_timing(&cfg, &timing_error) == 0);
    assert(cfg.timing_profile == X68K_TIMING_PROFILE_NONE);
    char *bus_wait_bad_argv[] = {"x68000", "-no-config", "-bus-wait", "bad"};
    assert(load_config(4, bus_wait_bad_argv, &cfg, &opt) != 0);
    FILE *bus_wait_file = fopen("bus_wait_config_test.cfg", "w");
    assert(bus_wait_file);
    fputs("bus_wait = x68030\n", bus_wait_file);
    fclose(bus_wait_file);
    char *bus_wait_file_argv[] = {"x68000", "-config", "bus_wait_config_test.cfg"};
    assert(load_config(3, bus_wait_file_argv, &cfg, &opt) != 0);
    assert(remove("bus_wait_config_test.cfg") == 0);

    /* 旧 bus_wait は移行先つきで拒否する。 */
    x68k_config_error_t err;
    char msg[512];
    char *xvi_bus_wait_argv[] = {"x68000", "-no-config", "-bus-wait", "xvi-16"};
    assert(x68k_config_load_from_args(4, xvi_bus_wait_argv, &cfg, &opt, &err) != 0);
    assert(err.code == X68K_CONFIG_ERROR_LEGACY_BUS_WAIT);
    x68k_config_format_error(&err, msg, sizeof(msg));
    assert(strstr(msg, "series = xvi を指定"));
    char *legacy_old_argv[] = {"x68000", "-no-config", "-bus-wait", "legacy"};
    assert(x68k_config_load_from_args(4, legacy_old_argv, &cfg, &opt, &err) != 0);
    x68k_config_format_error(&err, msg, sizeof(msg));
    assert(strstr(msg, "legacy") && strstr(msg, "x68030"));

    /* profile と cpu_model/cpu_clock の対応。不一致は拒否し、none だけ未対応 CPU を許す。 */
    static const struct {
        const char *profile, *model, *clock;
        int ok;
        mc68k_model_t model_out;
        int clock_out;
    } resolve_cases[] = {
        {"x68000", "68000", "10", 1, MC68K_MODEL_68000, 10},
        {"x68000", "68000", "0", 1, MC68K_MODEL_68000, 10},
        {"xvi", "68000", "16", 1, MC68K_MODEL_68000, 16},
        {"xvi", "68000", "10", 0, 0, 0},
        {"x68030", "68030", "25", 1, MC68K_MODEL_68030, 25},
        {"x68030", "68ec030", "25", 1, MC68K_MODEL_68EC030, 25},
        {"x68030", "68000", "25", 0, 0, 0},
        {"x68030", "68ec030", "33", 0, 0, 0},
        {"060turbo", "68060", "50", 1, MC68K_MODEL_68060, 50},
        {"060turbo", "68040", "50", 0, 0, 0},
        {"040turbo", "68040", "25", 1, MC68K_MODEL_68040, 25},
        {"040turbo", "68040", "33", 1, MC68K_MODEL_68040, 33},
        {"040turbo", "68040", "50", 0, 0, 0},
        {"040turbo", "68060", "25", 0, 0, 0},
        {"x68000", "68030", "10", 0, 0, 0},
        {"x68000", "68040", "10", 0, 0, 0},
        {"none", "68040", "33", 1, MC68K_MODEL_68040, 33},
        {"none", "68020", "10", 1, MC68K_MODEL_68020, 10},
        {"none", "68000", "20", 0, 0, 0},
    };
    for (unsigned i = 0; i < sizeof(resolve_cases) / sizeof(resolve_cases[0]); i++) {
        const char *profile = resolve_cases[i].profile;
        const bool bus_off = strcmp(profile, "none") == 0;
        const bool turbo = strstr(profile, "turbo") != NULL;
        /* 060turbo / 040turbo は series = x68030 + accelerator、none は bus_timing = off。 */
        const char *series = turbo ? "x68030" : (bus_off ? "x68000" : profile);
        char *rargv[] = {"x68000",       "-no-config",
                         "-series",      (char *)series,
                         "-accelerator", turbo ? (char *)profile : "none",
                         "-bus-timing",  bus_off ? "off" : "on",
                         "-cpu-model",   (char *)resolve_cases[i].model,
                         "-cpu-clock",   (char *)resolve_cases[i].clock};
        assert(x68k_config_load_from_args(12, rargv, &cfg, &opt, &err) == 0);
        int rc = x68k_config_resolve_timing(&cfg, &err);
        assert((rc == 0) == resolve_cases[i].ok);
        if (resolve_cases[i].ok) {
            assert(cfg.cpu_model == resolve_cases[i].model_out &&
                   cfg.cpu_clock_mhz == resolve_cases[i].clock_out);
        } else {
            assert(err.code == X68K_CONFIG_ERROR_TIMING_MISMATCH ||
                   err.code == X68K_CONFIG_ERROR_CPU_CLOCK);
        }
    }
    {
        char *a[] = {"x68000", "-no-config", "-cpu-model", "68040"};
        assert(x68k_config_load_from_args(4, a, &cfg, &opt, &err) == 0);
        assert(x68k_config_resolve_timing(&cfg, &err) != 0);
        assert(err.code == X68K_CONFIG_ERROR_TIMING_MISMATCH);
        assert(err.profile == X68K_TIMING_PROFILE_X68000 &&
               err.expected_model == MC68K_MODEL_68000 && err.expected_clock_mhz == 10 &&
               err.actual_model == MC68K_MODEL_68040 && err.actual_clock_mhz == 10);
        x68k_config_format_error(&err, msg, sizeof(msg));
        assert(strstr(msg, "x68000") && strstr(msg, "68000") && strstr(msg, "10 MHz") &&
               strstr(msg, "68040") && strstr(msg, "bus_timing"));
    }

    /* accelerator は X68030 専用。省略時は model / clock が自動で入る。 */
    {
        char *a[] = {"x68000", "-no-config", "-series", "xvi", "-accelerator", "040turbo"};
        assert(x68k_config_load_from_args(6, a, &cfg, &opt, &err) == 0);
        assert(x68k_config_resolve_timing(&cfg, &err) != 0);
        assert(err.code == X68K_CONFIG_ERROR_ACCELERATOR_SERIES);
        char *b[] = {"x68000", "-no-config", "-series", "x68030", "-accelerator", "040turbo"};
        assert(x68k_config_load_from_args(6, b, &cfg, &opt, &err) == 0);
        assert(x68k_config_resolve_timing(&cfg, &err) == 0);
        assert(cfg.timing_profile == X68K_TIMING_PROFILE_040TURBO);
        assert(cfg.cpu_model == MC68K_MODEL_68040 && cfg.cpu_clock_mhz == 25);
        char *c[] = {"x68000", "-no-config", "-series", "x68030", "-accelerator", "060turbo"};
        assert(x68k_config_load_from_args(6, c, &cfg, &opt, &err) == 0);
        assert(x68k_config_resolve_timing(&cfg, &err) == 0);
        assert(cfg.timing_profile == X68K_TIMING_PROFILE_060TURBO);
        assert(cfg.cpu_model == MC68K_MODEL_68060 && cfg.cpu_clock_mhz == 50);
    }

    /* 不正な cpu_clock / cpu_model / series 等は既定値へ戻さず構文エラーにする。 */
    static const struct {
        const char *flag, *value;
        x68k_config_error_code_t code;
    } bad_cases[] = {
        {"-cpu-clock", "abc", X68K_CONFIG_ERROR_CPU_CLOCK},
        {"-cpu-clock", "10x", X68K_CONFIG_ERROR_CPU_CLOCK},
        {"-cpu-clock", "-5", X68K_CONFIG_ERROR_CPU_CLOCK},
        {"-cpu-clock", "", X68K_CONFIG_ERROR_CPU_CLOCK},
        {"-cpu-model", "68099", X68K_CONFIG_ERROR_CPU_MODEL},
        {"-series", "legacy", X68K_CONFIG_ERROR_TIMING_PROFILE},
        {"-series", "none", X68K_CONFIG_ERROR_TIMING_PROFILE},
        {"-bus-timing", "maybe", X68K_CONFIG_ERROR_BUS_TIMING},
        {"-accelerator", "030turbo", X68K_CONFIG_ERROR_ACCELERATOR},
        {"-fpu", "bad", X68K_CONFIG_ERROR_FPU},
        {"-local-memory", "7", X68K_CONFIG_ERROR_LOCAL_MEMORY},
    };
    for (unsigned i = 0; i < sizeof(bad_cases) / sizeof(bad_cases[0]); i++) {
        char *bargv[] = {"x68000", "-no-config", (char *)bad_cases[i].flag,
                         (char *)bad_cases[i].value};
        assert(x68k_config_load_from_args(4, bargv, &cfg, &opt, &err) != 0);
        assert(err.code == bad_cases[i].code);
    }
    {
        char *a[] = {"x68000", "-no-config", "-cpu-clock"};
        assert(x68k_config_load_from_args(3, a, &cfg, &opt, &err) != 0);
        assert(err.code == X68K_CONFIG_ERROR_CPU_CLOCK);
        char *b[] = {"x68000", "-no-config", "-cpu-model"};
        assert(x68k_config_load_from_args(3, b, &cfg, &opt, &err) != 0);
        assert(err.code == X68K_CONFIG_ERROR_CPU_MODEL);
        char *c[] = {"x68000", "-no-config", "-bus-timing", "off", "-cpu-clock", "20"};
        assert(x68k_config_load_from_args(6, c, &cfg, &opt, &err) == 0);
        assert(x68k_config_resolve_timing(&cfg, &err) != 0);
        assert(err.code == X68K_CONFIG_ERROR_CPU_CLOCK && strcmp(err.value, "20") == 0);
    }
    FILE *bad_clock_file = fopen("bad_clock_config_test.cfg", "w");
    assert(bad_clock_file);
    fputs("cpu_clock = 25x\n", bad_clock_file);
    fclose(bad_clock_file);
    char *bad_clock_argv[] = {"x68000", "-config", "bad_clock_config_test.cfg"};
    assert(x68k_config_load_from_args(3, bad_clock_argv, &cfg, &opt, &err) != 0);
    assert(err.code == X68K_CONFIG_ERROR_CPU_CLOCK);
    assert(remove("bad_clock_config_test.cfg") == 0);

    FILE *local_memory_file = fopen("local_memory_config_test.cfg", "w");
    assert(local_memory_file);
    fputs("cpu_model = 68060\nlocal_memory = 64\n", local_memory_file);
    fclose(local_memory_file);
    char *local_memory_argv[] = {"x68000", "-config", "local_memory_config_test.cfg",
                                 "-local-memory", "128"};
    assert(load_config(5, local_memory_argv, &cfg, &opt) == 0);
    assert(cfg.local_memory_mb == 128);
    char *local_memory_off_argv[] = {"x68000", "-no-config", "-local-memory", "0"};
    assert(load_config(4, local_memory_off_argv, &cfg, &opt) == 0);
    assert(cfg.local_memory_mb == 0);

    FILE *fpu_file = fopen("fpu_config_test.cfg", "w");
    assert(fpu_file);
    fputs("fpu = 68881-secondary\n", fpu_file);
    fclose(fpu_file);
    char *fpu_file_argv[] = {"x68000", "-config", "fpu_config_test.cfg"};
    assert(load_config(3, fpu_file_argv, &cfg, &opt) == 0);
    assert(cfg.fpu == X68K_FPU_68881_SECONDARY);

    char *fpu_cli_argv[] = {"x68000", "-config", "fpu_config_test.cfg", "-fpu", "68881"};
    assert(load_config(5, fpu_cli_argv, &cfg, &opt) == 0);
    assert(cfg.fpu == X68K_FPU_68881);

    /* 複数 -config: 指定順に読み、重複キーは後のファイルが上書きする。 */
    FILE *multi_a = fopen("multi_a_config_test.cfg", "w");
    assert(multi_a);
    fputs("fd0 = a0.xdf\nfd1 = a1.xdf\nfpu = 68881\n", multi_a);
    fclose(multi_a);
    FILE *multi_b = fopen("multi_b_config_test.cfg", "w");
    assert(multi_b);
    fputs("fd1 = b1.xdf\ncpu_model = 68060\n", multi_b);
    fclose(multi_b);
    char *multi_argv[] = {"x68000", "-config", "multi_a_config_test.cfg", "-config",
                          "multi_b_config_test.cfg"};
    assert(load_config(5, multi_argv, &cfg, &opt) == 0);
    assert(strcmp(cfg.fd_path[0], "a0.xdf") == 0);
    assert(strcmp(cfg.fd_path[1], "b1.xdf") == 0);
    assert(cfg.fpu == X68K_FPU_68881);
    assert(cfg.cpu_model == MC68K_MODEL_68060);
    assert(remove("multi_a_config_test.cfg") == 0);
    assert(remove("multi_b_config_test.cfg") == 0);

    char *fpcp_cli_argv[] = {"x68000", "-no-config", "-fpu", "68882"};
    assert(load_config(4, fpcp_cli_argv, &cfg, &opt) == 0);
    assert(cfg.fpu == X68K_FPU_68882);
    char *cpu040_cli_argv[] = {"x68000", "-no-config", "-cpu-model", "68040"};
    assert(load_config(4, cpu040_cli_argv, &cfg, &opt) == 0);
    assert(cfg.cpu_model == MC68K_MODEL_68040);
    FILE *cpu040_file = fopen("cpu040_config_test.cfg", "w");
    assert(cpu040_file);
    fputs("cpu_model = 68040\n", cpu040_file);
    fclose(cpu040_file);
    char *cpu040_file_argv[] = {"x68000", "-config", "cpu040_config_test.cfg"};
    assert(load_config(3, cpu040_file_argv, &cfg, &opt) == 0);
    assert(cfg.cpu_model == MC68K_MODEL_68040);
    char *cpu060_cli_argv[] = {"x68000", "-no-config", "-cpu-model", "68060"};
    assert(load_config(4, cpu060_cli_argv, &cfg, &opt) == 0);
    assert(cfg.cpu_model == MC68K_MODEL_68060);
    char *fpcp_bad_argv[] = {"x68000", "-no-config", "-fpu", "bogus"};
    assert(load_config(4, fpcp_bad_argv, &cfg, &opt) != 0);
    assert(remove("fpu_config_test.cfg") == 0);
    assert(remove("cpu040_config_test.cfg") == 0);
    assert(remove("local_memory_config_test.cfg") == 0);

#ifndef _WIN32
    /* 基準ディレクトリが '\\' 区切り（Windowsのユーザー領域）なら、設定の相対部分の '/' も
     * '\\' にそろえる。'\\' をファイル名の一部として扱える環境で、接頭辞連結のまま検証する。 */
    {
        const char *win_dir = "cfgwin\\ix68k\\";
        char *win_argv[] = {"x68000"};
        x68k_config_set_search_dirs(NULL, NULL, win_dir);
        FILE *win_file = fopen("cfgwin\\ix68k\\ix68k.cfg", "w");
        assert(win_file);
        fputs("rom = rom/IPLROM.DAT\nfd0 = C:/abs/disk.xdf\n", win_file);
        fclose(win_file);
        assert(load_config(1, win_argv, &cfg, &opt) == 0);
        assert(strcmp(cfg.rom_path, "cfgwin\\ix68k\\rom\\IPLROM.DAT") == 0);
        assert(strcmp(cfg.fd_path[0], "C:/abs/disk.xdf") == 0);
        assert(remove("cfgwin\\ix68k\\ix68k.cfg") == 0);
    }
#endif

    /* 標準設定ファイルの探索: カレント → 実行ファイルの隣 → ユーザー領域。
     * ディレクトリは接頭辞として連結されるため、ファイル名接頭辞で代用する。 */
    {
        const char *exe_dir = "cfgsearch_exe_";
        const char *user_dir = "cfgsearch_user_";
        char *search_argv[] = {"x68000"};
        x68k_config_set_search_dirs(exe_dir, NULL, user_dir);

        FILE *user_file = fopen("cfgsearch_user_ix68k.cfg", "w");
        assert(user_file);
        fputs("rom = rom/IPLROM.DAT\nfd0 = /abs/disk.xdf\ncpu_model = 68040\n", user_file);
        fclose(user_file);
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(strcmp(cfg.rom_path, "cfgsearch_user_rom/IPLROM.DAT") == 0);
        assert(strcmp(cfg.sram_path, "cfgsearch_user_sram.dat") == 0);
        assert(strcmp(cfg.fd_path[0], "/abs/disk.xdf") == 0);
        assert(cfg.fd_path[1][0] == '\0');
        assert(cfg.cpu_model == MC68K_MODEL_68040);

        FILE *exe_file = fopen("cfgsearch_exe_ix68k.cfg", "w");
        assert(exe_file);
        fputs("rom = exe_rom.dat\n", exe_file);
        fclose(exe_file);
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(strcmp(cfg.rom_path, "cfgsearch_exe_exe_rom.dat") == 0);

        /* CLI の指定は設定ファイルの場所で書き換えない。 */
        char *search_cli_argv[] = {"x68000", "-rom", "cli.rom"};
        assert(load_config(3, search_cli_argv, &cfg, &opt) == 0);
        assert(strcmp(cfg.rom_path, "cli.rom") == 0);

        /* カレントディレクトリの ix68k.cfg が最優先で、パスは書き換えない。 */
        FILE *cwd_file = fopen("ix68k.cfg", "w");
        assert(cwd_file);
        fputs("rom = cwd_rom.dat\n", cwd_file);
        fclose(cwd_file);
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(strcmp(cfg.rom_path, "cwd_rom.dat") == 0);
        assert(strcmp(cfg.sram_path, "sram.dat") == 0);
        assert(remove("ix68k.cfg") == 0);

        /* -config 明示時は探索しない。 */
        char *search_explicit_argv[] = {"x68000", "-config", "cfgsearch_missing.cfg"};
        x68k_config_error_t explicit_error;
        assert(x68k_config_load_from_args(3, search_explicit_argv, &cfg, &opt,
                                          &explicit_error) != 0);
        assert(explicit_error.code == X68K_CONFIG_ERROR_FILE_OPEN);
        assert(cfg.rom_path[0] == '\0');

        /* 連結後に長すぎるパスはエラーにする。 */
        char long_line[600] = "rom = ";
        memset(long_line + 6, 'a', 500);
        strcpy(long_line + 506, ".dat\n");
        FILE *long_file = fopen("cfgsearch_exe_ix68k.cfg", "w");
        assert(long_file);
        fputs(long_line, long_file);
        fclose(long_file);
        x68k_config_error_t long_error;
        assert(x68k_config_load_from_args(1, search_argv, &cfg, &opt, &long_error) != 0);
        assert(long_error.code == X68K_CONFIG_ERROR_PATH_TOO_LONG);

        /* 任意の名前のファイル探索（keyboard.cfg 用）。カレント → 実行ファイルの隣 →
         * ユーザー領域。 */
        char found[64];
        assert(x68k_config_find_file("cfgsearch_kb.cfg", found, sizeof(found)) == 0);
        FILE *kb_user = fopen("cfgsearch_user_cfgsearch_kb.cfg", "w");
        assert(kb_user);
        fclose(kb_user);
        assert(x68k_config_find_file("cfgsearch_kb.cfg", found, sizeof(found)) == 1);
        assert(strcmp(found, "cfgsearch_user_cfgsearch_kb.cfg") == 0);
        FILE *kb_exe = fopen("cfgsearch_exe_cfgsearch_kb.cfg", "w");
        assert(kb_exe);
        fclose(kb_exe);
        assert(x68k_config_find_file("cfgsearch_kb.cfg", found, sizeof(found)) == 1);
        assert(strcmp(found, "cfgsearch_exe_cfgsearch_kb.cfg") == 0);
        FILE *kb_cwd = fopen("cfgsearch_kb.cfg", "w");
        assert(kb_cwd);
        fclose(kb_cwd);
        assert(x68k_config_find_file("cfgsearch_kb.cfg", found, sizeof(found)) == 1);
        assert(strcmp(found, "cfgsearch_kb.cfg") == 0);
        char tiny[8];
        assert(x68k_config_find_file("cfgsearch_kb.cfg", tiny, sizeof(tiny)) == 0);
        assert(remove("cfgsearch_kb.cfg") == 0);
        assert(remove("cfgsearch_exe_cfgsearch_kb.cfg") == 0);
        assert(remove("cfgsearch_user_cfgsearch_kb.cfg") == 0);

        assert(remove("cfgsearch_exe_ix68k.cfg") == 0);
        assert(remove("cfgsearch_user_ix68k.cfg") == 0);
        assert(x68k_config_loaded_dir() != NULL);

        /* 設定がどこにも無い: 引数なし起動ならユーザー領域に既定設定を作り、読み込む。 */
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(x68k_config_default_created() == 1);
        assert(strcmp(x68k_config_loaded_dir(), user_dir) == 0);
        assert(strcmp(cfg.rom_path, "cfgsearch_user_rom/IPLROM.DAT") == 0);
        assert(strcmp(cfg.cgrom_path, "cfgsearch_user_rom/CGROM.DAT") == 0);
        assert(strcmp(cfg.sram_path, "cfgsearch_user_sram.dat") == 0);
        /* 自動作成された設定は、配布する profiles/ix68k.cfg.example と同一内容。 */
        assert(files_identical("cfgsearch_user_ix68k.cfg", IX68K_CFG_EXAMPLE_PATH));
        /* 2回目以降は作らず、作成済みの設定を読む。 */
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(x68k_config_default_created() == 0);
        assert(strcmp(cfg.rom_path, "cfgsearch_user_rom/IPLROM.DAT") == 0);
        assert(remove("cfgsearch_user_ix68k.cfg") == 0);

        /* CLI 引数あり・-no-config・-config 指定のときは作らない。 */
        assert(load_config(3, search_cli_argv, &cfg, &opt) == 0);
        assert(x68k_config_default_created() == 0);
        assert(x68k_config_loaded_dir() == NULL);
        FILE *probe = fopen("cfgsearch_user_ix68k.cfg", "r");
        assert(probe == NULL);
        char *search_noconfig_argv[] = {"x68000", "-no-config"};
        assert(load_config(2, search_noconfig_argv, &cfg, &opt) == 0);
        assert(x68k_config_default_created() == 0);
        assert(fopen("cfgsearch_user_ix68k.cfg", "r") == NULL);

        /* ユーザー領域が無ければ作れず、何も読まない。 */
        x68k_config_set_search_dirs(exe_dir, NULL, NULL);
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(x68k_config_default_created() == 0);
        assert(cfg.rom_path[0] == '\0');
        /* 設定領域とデータ領域を分ける（Linux）:
         * 設定は設定領域から探し、相対パスはデータ領域基準。
         * 設定領域の設定が、同名のデータ領域の設定より優先される。 */
        const char *config_dir = "cfgsearch_cfg_";
        x68k_config_set_search_dirs(exe_dir, config_dir, user_dir);
        FILE *split_data = fopen("cfgsearch_user_ix68k.cfg", "w");
        assert(split_data);
        fputs("rom = from_data.dat\n", split_data);
        fclose(split_data);
        FILE *split_cfg = fopen("cfgsearch_cfg_ix68k.cfg", "w");
        assert(split_cfg);
        fputs("rom = rom/IPLROM.DAT\n", split_cfg);
        fclose(split_cfg);
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(strcmp(cfg.rom_path, "cfgsearch_user_rom/IPLROM.DAT") == 0);
        assert(strcmp(x68k_config_loaded_dir(), user_dir) == 0);
        assert(strcmp(x68k_config_loaded_path(), "cfgsearch_cfg_ix68k.cfg") == 0);
        assert(remove("cfgsearch_cfg_ix68k.cfg") == 0);
        /* 設定領域に無ければデータ領域の設定を読む。 */
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(strcmp(cfg.rom_path, "cfgsearch_user_from_data.dat") == 0);
        assert(remove("cfgsearch_user_ix68k.cfg") == 0);
        /* どこにも無ければ、既定設定は設定領域に作り、相対パスはデータ領域基準で解決する。 */
        assert(load_config(1, search_argv, &cfg, &opt) == 0);
        assert(x68k_config_default_created() == 1);
        assert(strcmp(x68k_config_loaded_path(), "cfgsearch_cfg_ix68k.cfg") == 0);
        assert(strcmp(cfg.rom_path, "cfgsearch_user_rom/IPLROM.DAT") == 0);
        assert(fopen("cfgsearch_user_ix68k.cfg", "r") == NULL);
        assert(remove("cfgsearch_cfg_ix68k.cfg") == 0);
        /* keyboard.cfg も設定領域を探す。 */
        char kb_found[64];
        FILE *kb_cfg = fopen("cfgsearch_cfg_kb2.cfg", "w");
        assert(kb_cfg);
        fclose(kb_cfg);
        assert(x68k_config_find_file("kb2.cfg", kb_found, sizeof(kb_found)) == 1);
        assert(strcmp(kb_found, "cfgsearch_cfg_kb2.cfg") == 0);
        assert(remove("cfgsearch_cfg_kb2.cfg") == 0);
        x68k_config_set_search_dirs(NULL, NULL, NULL);
    }
    puts("config_test: OK");
    return 0;
}
