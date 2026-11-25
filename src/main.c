#include "platform/platform.h"
#include "bus/x68k_bus.h"
#include "render/renderer.h"
#include "audio/audio_mixer.h"
#include "app/config.h"
#include "app/fpu_config.h"
#include "app/keyboard_config.h"
#include "app/rom_check.h"
#include "app/checkpoint.h"
#include "app/frame_timing.h"
#include "x68k_version.h"
#include "mc68000/cpu.h"
#include "mc68000/cpu_types.h"
#include "mc68000/debug.h"
#include "mcp/debugger.h"
#include "mcp/perf.h"
#include "mcp/sock_path.h"
#include "mcp/port.h"
#include "mcp/text_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#ifdef _WIN32
#include "platform/win_compat.h"
#else
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "app_internal.h"
#include "ui/disk_window.h"
#include "ui/keyboard_window.h"
x68k_app_t s_app = {.native_w = 512, .native_h = 512};
/* 表示1ドットの横幅（x68k_bus_get_dot_width_units()）のキャッシュ。
 * 初期値 3 は _CRTMOD 未実行時のフォールバックである 512x512 モードの値。 */
static int s_native_dot_units = 3;
/* メニュー「View」で選択中の表示倍率。基準サイズ 768x512 が
 * ホストの画面に対して小さいときにユーザーが手動で選ぶ。実機に無い機能なので
 * SRAMには保存しない。実際の初期値は main() が cfg.window_zoom_x10 から与える。 */
static float s_window_zoom = 1.0f;
static uint64_t s_boot_time_us = 0; /* $9CA システムワーク更新用: 起動時刻(us) */

/* CPUクロック（実機標準10MHz + 高速化オプション16/25/33/50MHz）。
 * -cpu-clock 起動オプションまたは設定ファイルの cpu_clock で起動時に決まり、
 * 実行中は変更しない。 */
static uint32_t s_cpu_clock_hz = 10000000u;

/* メニュー「View」の表示倍率テーブル(x10表記、PAL_AUX_HOTKEY_ZOOM_1〜
 * ZOOM_2の連番に対応)。起動時の倍率は window_scale / -window-scale。 */
static const int kWindowZoomX10[] = {X68K_WINDOW_ZOOM_X10_1, X68K_WINDOW_ZOOM_X10_1_5,
                                     X68K_WINDOW_ZOOM_X10_2};
#define WINDOW_ZOOM_X10_SCALE 10.0f /* kWindowZoomX10 は倍率を10倍した整数 */

/* ラスタ同期は CLI だけで切り替える。
 * -raster-y live は縦スクロール値のフレーム先頭ラッチを無効にする。 */
static int s_raster_y_live = 0;
static int s_raster_pal_live = 0;

/* MFP VR の S ビットによるソフトウェア EOI。CLI でのみ有効化する。 */
static int s_mfp_soft_eoi_enabled = 0;

/* GPIP bit7(H-SYNC)/bit4(V-DISP)を CRTC ラスターモデルから合成する。 */
static int s_mfp_gpip_raster_enabled = 1;

static int checkpoint_auto_input_pending(void) {
    return app_type_pending() || s_app.key_input_paste_active;
}

static void checkpoint_host_cleanup(void) {
    pal_audio_reset();
    iocs_key_wait_reset();
    s_app.key_input_waiting = 0;
    s_app.iocs_ring_cnt = 0;
    s_app.iocs_ring_pos = 0;
    mcp_text_events_reset();
}

/* フレームペーシングの取り戻し上限(マイクロ秒)。ホスト側でメインループが
 * 長時間止まった（ウィンドウ操作・スリープ等）後に遅れた分を全速で
 * 取り戻すと、その間だけCRTC周期を超える速度で音声を生成してSDLの出力キューに
 * 滞留（＝恒久的な音の遅延）を作る。これを超えて遅れたら取り戻しを
 * 諦めて現在時刻へ同期し直す。 */
#define FRAME_CATCHUP_LIMIT_US 100000ULL
#define MFP_TICK_CYCLES        200

/* MFPのチャンク内同期用。debug_run経路は命令ごとに0へ戻るため同期しない（0を返す）。 */
static int mfp_cycle_source(void *ctx) {
    if (dbg_last_exec_used_debug_run())
        return 0;
    return mc68k_executed_cycles((const mc68k_state_t *)ctx);
}

/* DMACのチャンク内同期用。68060のbus callback内でだけ有効な
 * mc68k_bus_access_cycles()の値を返す。debug_run経路は呼出しごとに0へ戻るため同期しない。 */
static bool dmac_cycle_source(void *ctx, uint64_t *out) {
    if (dbg_last_exec_used_debug_run())
        return false;
    return mc68k_bus_access_cycles((const mc68k_state_t *)ctx, out);
}

/* CPU が自力では前進できない状態か（STOP 命令待機・ダブルバスフォルト停止）。
 * この間も周辺（MFP タイマ等）は実時間で進める必要がある。 */
static int cpu_is_waiting(const mc68k_state_t *cpu) {
    cpu_exec_state_t st = mc68k_get_exec_state(cpu);
    return st == CPU_STOPPED || st == CPU_HALTED;
}

/* ADPCM は固定出力レートのステレオへ変換して SDL へキューする。
 * バッファ容量は「1/60秒ぶんの出力フレーム数」に十分な余裕を持たせる
 * （リサンプル後の量はCRTCフレーム周期に比例する）。ADPCM_AUDIO_RING_SIZE(1024)が
 * 入力側の上限。 */
#define AUDIO_OUT_RATE_HZ  44100u
#define AUDIO_OUT_CHANNELS 2

/* OPM は 62500Hz 固定のステレオ int32 を生成し、出力レートへリサンプルして
 * からADPCM(int32へ変換済み)と加算する。フレームごとの必要量はCRTC周期から導く。 */
#define OPM_GEN_RATE_HZ 62500u
static void audio_frame_buffers_destroy(audio_frame_buffers_t *buffers) {
    free(buffers->opm_raw);
    free(buffers->opm_out);
    free(buffers->adpcm_out);
    free(buffers->adpcm_tmp);
    free(buffers->adpcm_fifo);
    free(buffers->mono);
    free(buffers->mixed);
    memset(buffers, 0, sizeof(*buffers));
}

/* CRTC周期に対応するフレーム単位の音声作業領域を確保する。すべての割当が成功してから
 * 古い領域を置換するため、失敗時も前回の有効領域を壊さない。 */
static int audio_frame_buffers_reserve(audio_frame_buffers_t *buffers, size_t frames) {
    if (frames <= buffers->frames)
        return 1;
    if (frames > SIZE_MAX / 4u || frames * 2u > (size_t)INT32_MAX)
        return 0;
    /* ADPCM出力FIFOの容量(ステレオフレーム数)。ADPCMの1フレーム自然出力数は
     * OPM由来の要求数`no`と±数サンプルずれる(入力m・native_rate・noが各々整数
     * ジッタするため)。このFIFOでその差分を吸収し、毎フレーム正確に`no`フレーム
     * を取り出す。差分は平均ゼロのランダムウォークで小さく収まるが、起動直後や
     * レート変化時のバーストに備え1フレーム分+αの余裕を持たせる。 */
    size_t fifo_frames = frames * 2u + 256u;
    if (fifo_frames < frames || fifo_frames > SIZE_MAX / (sizeof(int32_t) * 2u))
        return 0;

    audio_frame_buffers_t next = {0};
    next.frames = frames;
    next.fifo_frames = fifo_frames;
    next.opm_raw = malloc(sizeof(int32_t) * frames * 2u);
    next.opm_out = malloc(sizeof(int32_t) * frames * 2u);
    next.adpcm_out = malloc(sizeof(int32_t) * frames * 2u);
    next.adpcm_tmp = malloc(sizeof(int32_t) * frames * 2u);
    next.adpcm_fifo = calloc(fifo_frames * 2u, sizeof(int32_t));
    next.mono = malloc(sizeof(int16_t) * frames);
    next.mixed = malloc(sizeof(int16_t) * frames * 2u);
    if (!next.opm_raw || !next.opm_out || !next.adpcm_out || !next.adpcm_tmp ||
        !next.adpcm_fifo || !next.mono || !next.mixed) {
        audio_frame_buffers_destroy(&next);
        return 0;
    }
    audio_frame_buffers_destroy(buffers);
    *buffers = next;
    return 1;
}

static void audio_discard_opm(x68k_bus_t *bus, int frames) {
    int32_t discarded[64 * 2];
    while (frames > 0) {
        int count = frames > 64 ? 64 : frames;
        x68k_bus_opm_generate(bus, discarded, count);
        frames -= count;
    }
}

/* バッファ確保に失敗したフレームのADPCMを捨てる。回復時に過去の音を再生しない。 */
static void audio_discard_adpcm(x68k_bus_t *bus) {
    int16_t discarded[64];
    uint32_t native_rate_hz;
    int left_on, right_on;
    for (;;) {
        int taken = x68k_bus_adpcm_take_samples(bus, discarded,
                                                (int)(sizeof(discarded) / sizeof(discarded[0])),
                                                &native_rate_hz, &left_on, &right_on);
        if (taken == 0)
            break;
    }
}

static void audio_queue_silence(int frames) {
    int16_t silence[64 * AUDIO_OUT_CHANNELS] = {0};
    while (frames > 0) {
        int count = frames > 64 ? 64 : frames;
        pal_audio_queue(silence, count);
        frames -= count;
    }
}

/* マイク入力は固定 44100Hz モノラルでキャプチャデバイスを開き、
 * ADPCMネイティブレート(3.9k-15.6kHz)へダウンサンプルしてからpushする。 */
#define AUDIO_CAPTURE_RATE_HZ     44100u
#define AUDIO_CAPTURE_CHANNELS    1
#define AUDIO_CAPTURE_BUF_SAMPLES 1024 /* 44100Hz側: 約735サンプル/フレームに余裕 */
#define AUDIO_REC_BUF_SAMPLES     512 /* ダウンサンプル後(ネイティブレート)側: 出力と同じ余裕 */

/*
 * ピクセルアスペクト比（PAR）補正。768x512（_CRTMOD 16）を基準サイズとし、
 * 高さは常に WINDOW_BASE_H で固定、幅は表示1ドットの横幅
 * （x68k_bus_get_dot_width_units()、768x512モードのドットを2とする単位）から
 * 求める。画面モードを切り替えてもウィンドウサイズが変動しないようにするため。
 *
 * 512×512  (units 3) → 768×512
 * 256×256  (units 6) → 768×512
 * 768×512  (units 2) → 768×512   (基準)
 * 1024×424 (units 2) → 1024×512  (_CRTMOD 17、24.70kHz)
 * 1024×848 (units 2) → 1024×512  (_CRTMOD 18、24.70kHz インターレース)
 * 256×256  (units 4) → 512×512   (CRTC直叩きの正方形ドット、俗称 "384x256"）
 */
#define WINDOW_BASE_H        512 /* 基準ウィンドウ高さ = 768x512 モードの表示高さ */
#define DOT_WIDTH_UNITS_BASE 2   /* 768x512 モードの1ドット分の units 値 */

static void par_corrected(int native_w, int dot_width_units, int *display_w, int *display_h) {
    *display_w = native_w * dot_width_units / DOT_WIDTH_UNITS_BASE;
    *display_h = WINDOW_BASE_H;
}

/* par_corrected() の結果にメニュー「View」の表示倍率(s_window_zoom)
 * を掛ける。 */
static void apply_window_zoom(int *display_w, int *display_h, float zoom) {
    *display_w = (int)((float)*display_w * zoom + 0.5f);
    *display_h = (int)((float)*display_h * zoom + 0.5f);
}

/* platform 層のキー入力コールバックを bus へ接続する。 */
static void mfp_usart_rx_push_cb(void *ctx, uint8_t byte) {
    x68k_bus_mfp_usart_rx_push((x68k_bus_t *)ctx, byte);
}

static void mfp_usart_rx_clear_cb(void *ctx) {
    x68k_bus_mfp_usart_rx_clear((x68k_bus_t *)ctx);
}

/* platform 層のマウス入力コールバックを bus へ接続する。 */
static void scc_mouse_position_cb(void *ctx, int x, int y) {
    x68k_bus_scc_mouse_position((x68k_bus_t *)ctx, x, y);
}

static void scc_mouse_button_cb(void *ctx, int left, int right) {
    x68k_bus_scc_mouse_button((x68k_bus_t *)ctx, left, right);
}

/* mc68000コアの mc68k_peek_byte/word/long_fn_t 型に合わせるラッパー
 * （非侵襲peek配線、MCPデバッガのメモリダンプ・逆アセンブル用）。 */
static uint8_t x68k_peek8_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek8((const x68k_bus_t *)ctx, addr);
}

static uint16_t x68k_peek16_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek16((const x68k_bus_t *)ctx, addr);
}

static uint32_t x68k_peek32_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek32((const x68k_bus_t *)ctx, addr);
}

#ifdef X68K_PGO_GENERATE
/* PGO 計装ビルド専用: SIGTERM で .profraw を書き出して終了する（学習の自動化用）。
 * 非同期シグナル安全ではないが、配布しない学習専用ビルドに限る。 */
extern int __llvm_profile_write_file(void);

static void pgo_sigterm_handler(int sig) {
    (void)sig;
    __llvm_profile_write_file();
    _exit(0);
}
#endif

int main(int argc, char *argv[]) {
#ifdef X68K_PGO_GENERATE
    signal(SIGTERM, pgo_sigterm_handler);
#endif
#ifndef _WIN32
    /* MCPクライアント切断中の送信でSIGPIPEが飛びプロセスが即死するのを防ぐ
     * （デフォルトはEPIPE errnoの代わりにプロセス終了）。WindowsにSIGPIPEは無く、
     * 切断はsend()の戻り値/WSAGetLastError()で検知するため対応不要。 */
    signal(SIGPIPE, SIG_IGN);
#endif

    s_app.boot_time = time(NULL);
    s_boot_time_us = pal_get_ticks_us();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("ix68k %s\n", X68K_VERSION_STRING);
            return 0;
        }
    }

    if (x68k_config_wants_help(argc, argv)) {
        puts(x68k_config_usage());
        return 0;
    }

    x68k_config_set_search_dirs(pal_get_exe_dir(), pal_get_user_config_dir(),
                                pal_get_user_dir());
    x68k_config_t cfg;
    x68k_cli_options_t cli_options = {app_runtime_raster_sync_enabled(),
                                      s_raster_y_live,
                                      s_raster_pal_live,
                                      s_mfp_soft_eoi_enabled,
                                      s_mfp_gpip_raster_enabled,
                                      1};
    x68k_config_error_t config_error;
    if (x68k_config_load_from_args(argc, argv, &cfg, &cli_options, &config_error) ||
        x68k_config_resolve_timing(&cfg, &config_error)) {
        char message[512];
        x68k_config_format_error(&config_error, message, sizeof(message));
        pal_show_error("ix68k", message);
        return 2;
    }
    mc68k_fpu_config_t fpu_config;
    if (!x68k_select_fpu_config(cfg.cpu_model, cfg.fpu, &fpu_config)) {
        pal_show_error("ix68k", "fpu の組合せが不正です（68000: off/on/68881/68881-secondary、"
                                "68ec030/68030: off/on/68882、68040/68060: off/on）。");
        return 2;
    }
    const char *config_dir = x68k_config_loaded_dir();
    if (x68k_config_default_created()) {
        /* 初回起動: ROM とディスクイメージの置き場所を、設定ファイルの隣に作る。 */
        static const char *const kUserSubdirs[] = {"rom", "disk"};
        for (size_t i = 0; i < sizeof(kUserSubdirs) / sizeof(kUserSubdirs[0]); i++) {
            char subdir[1024];
            snprintf(subdir, sizeof(subdir), "%s%s", config_dir, kUserSubdirs[i]);
            if (pal_create_dir(subdir) < 0)
                pal_log("ディレクトリを作成できません: %s\n", subdir);
        }
    }
    if (config_dir && cfg.rom_path[0]) {
        /* 実行ファイルの隣・ユーザー領域の設定を使う起動（.app / .desktop 等）では、
         * IPLROM が無いと何も表示されないため、置き場所を案内して終了する。 */
        FILE *rom_file = fopen(cfg.rom_path, "rb");
        if (!rom_file) {
            char msg[2048];
            snprintf(msg, sizeof(msg),
                     "IPLROM が見つかりません: %s\n\n"
                     "IPLROM と CGROM を次のフォルダに置いてください:\n  %srom\n"
                     "設定ファイル: %s",
                     cfg.rom_path, config_dir, x68k_config_loaded_path());
            pal_show_error("ix68k", msg);
            return 2;
        }
        fclose(rom_file);
    }
    if (!cfg.rom_path[0]) {
        /* ROM 無しではCRTCが未設定のまま縦長(256x512)のウィンドウになる。 */
        char msg[2048];
        snprintf(msg, sizeof(msg),
                 "IPLROM が指定されていません（-rom または設定ファイルの rom =）。\n\n%s",
                 x68k_config_usage());
        pal_show_error("ix68k", msg);
        /* 実行ファイルのダブルクリックは端末/コンソール内で起動され、終了と同時に
         * ウィンドウが閉じてエラーが読めない。引数なし起動ではキー入力を待つ。 */
        if (argc == 1 && isatty(STDIN_FILENO) && isatty(STDERR_FILENO)) {
            fputs("\nEnter キーを押すと終了します。", stderr);
            fflush(stderr);
            (void)getchar();
        }
        return 2;
    }
    {
        long rom_size = 0;
        const x68k_rom_check_result_t rom_result =
            x68k_rom_check(cfg.rom_path, cfg.timing_profile, cfg.cpu_model, &rom_size);
        if (rom_result != X68K_ROM_CHECK_OK) {
            char msg[2048];
            x68k_rom_check_format(rom_result, cfg.rom_path, rom_size, msg, sizeof(msg));
            pal_show_error("ix68k", msg);
            return 2;
        }
#ifndef X68K_HEADLESS
        /* ヘッドレス版は文字を描画しないため CGROM 未指定を許す。 */
        if (!cfg.cgrom_path[0] && x68k_cgrom_required(rom_size)) {
            char msg[2048];
            x68k_cgrom_error_format(NULL, msg, sizeof(msg));
            pal_show_error("ix68k", msg);
            return 2;
        }
#endif
    }
    app_runtime_set_raster_sync_enabled(cli_options.raster_sync_enabled);
    s_raster_y_live = cli_options.raster_y_live;
    s_raster_pal_live = cli_options.raster_pal_live;
    s_mfp_soft_eoi_enabled = cli_options.mfp_soft_eoi_enabled;
    s_mfp_gpip_raster_enabled = cli_options.mfp_gpip_raster_enabled;
    int frameskip_enabled = cli_options.frameskip_enabled;
    if (cfg.type_delay_sec <= 0)
        cfg.type_delay_sec = 20;

    s_cpu_clock_hz = (uint32_t)cfg.cpu_clock_mhz * 1000000u;

    /* ネイティブ解像度（CRTC R20 由来）とPAR補正後の表示解像度（ウィンドウサイズ）。
     * ネイティブ解像度は s_app.native_w / s_app.native_h に保持する
     * （フレームループ末尾のx68k_compose_frame 呼び出しから参照する）。 */
    int display_w, display_h;
    s_window_zoom = (float)cfg.window_zoom_x10 / WINDOW_ZOOM_X10_SCALE;
    par_corrected(s_app.native_w, s_native_dot_units, &display_w,
                  &display_h); /* 初期: 768×512 */
    apply_window_zoom(&display_w, &display_h, s_window_zoom);

    /* 複数ウィンドウを見分けるため、環境変数でインスタンスを指定している場合のみ
     * タイトルにインスタンス名を出す。未指定時は "ix68k"。
     * WindowsはTCP（IX68K_MCP_PORT）、macOS/LinuxはAF_UNIX（IX68K_MCP_SOCK）で
     * 由来の判定元が異なる（mcp_sock_path_source() はWindows経路では
     * 常にDEFAULTになり、この条件が常に偽になってしまう）。 */
    char window_title[64] = "ix68k";
#ifdef _WIN32
    if (mcp_port_source() == MCP_PORT_SRC_ENV)
        snprintf(window_title, sizeof(window_title), "ix68k [:%u]", (unsigned)mcp_port());
#else
    if (mcp_sock_path_source() == MCP_SOCK_SRC_ENV) {
        const char *sock = mcp_sock_path();
        if (sock) {
            const char *base = strrchr(sock, '/');
            base = base ? base + 1 : sock;
            const char *dot = strrchr(base, '.');
            int name_len = dot ? (int)(dot - base) : (int)strlen(base);
            snprintf(window_title, sizeof(window_title), "ix68k [%.*s]", name_len, base);
        }
    }
#endif

    pal_log("ix68k %s\n", X68K_VERSION_STRING);
    if (pal_init(window_title, display_w, display_h) < 0) {
        pal_log("pal_init failed\n");
        return 1;
    }
    /* pal_init はウィンドウとテクスチャを同サイズで作るが、テクスチャをネイティブに合わせ直す
     */
    pal_resize(s_app.native_w, s_app.native_h, display_w, display_h);
    x68k_keyboard_config_t keyboard_cfg;
    /* keyboard.cfg は ix68k.cfg と同じ探索先（カレントディレクトリ → 実行ファイルの隣 →
     * ユーザー領域）から探す。どこにも無ければ空パスとし、内蔵の既定値を使う。 */
    char keyboard_config_path[1024];
    if (!x68k_config_find_file("keyboard.cfg", keyboard_config_path,
                               sizeof(keyboard_config_path)))
        keyboard_config_path[0] = '\0';
    FILE *keyboard_config_file = fopen(keyboard_config_path, "r");
    if (keyboard_config_file) {
        fclose(keyboard_config_file);
        pal_log("keyboard.cfg loaded: %s\n", keyboard_config_path);
    } else {
        pal_log("keyboard.cfg not found: using built-in defaults\n");
    }
    x68k_keyboard_config_load(keyboard_config_path, cfg.keyboard_layout, &keyboard_cfg);
    pal_key_set_us_layout(strcmp(keyboard_cfg.layout, "us") == 0);
    pal_joystick_set_select_start(cfg.joystick_select_start);
    pal_joystick_set_pad(0, cfg.joystick_pad[0]);
    pal_joystick_set_pad(1, cfg.joystick_pad[1]);
    pal_hotkey_config_t hotkey_config = {
        .disk_window = keyboard_cfg.disk_window_hotkey,
        .software_keyboard = keyboard_cfg.software_keyboard_hotkey,
        .xf = {keyboard_cfg.xf_hotkey[0], keyboard_cfg.xf_hotkey[1], keyboard_cfg.xf_hotkey[2],
               keyboard_cfg.xf_hotkey[3], keyboard_cfg.xf_hotkey[4]},
        .kigou = keyboard_cfg.kigou_hotkey,
        .touroku = keyboard_cfg.touroku_hotkey,
        .break_key = keyboard_cfg.break_hotkey,
    };
    pal_key_set_hotkey_config(&hotkey_config);
    /* メニュー「View」チェックマーク初期値 */
    pal_aux_set_current_window_zoom_x10(cfg.window_zoom_x10);
    if (cfg.window_fullscreen && !pal_is_fullscreen())
        pal_toggle_fullscreen();

    /* 音声初期化に失敗した場合やヘッドレス環境（platform_null）では
     * pal_audio_available()==0 になり、以降のキューイングは自動的にno-opになる
     * （呼び出し側でのガード不要）。 */
    if (!pal_audio_init((int)AUDIO_OUT_RATE_HZ, AUDIO_OUT_CHANNELS))
        pal_log("Audio output unavailable (headless or SDL_INIT_AUDIO failure)\n");

    /* マイクはゲストが実際に録音を開始するまでキャプチャデバイス自体をopenしない
     * （メインループの mic_should_be_active 遷移箇所を参照。単にpausedで
     * 開くだけでもOSのマイク使用インジケータが点灯し続けてしまうため）。 */

    /* RS-232C(SCC ch A)のホスト接続は環境変数で有効化する。
     * X68K_RS232_PTY=1 または X68K_RS232_DEV=<path> を指定しない限り
     * キャリアなしで動作する。 */
    if (!pal_serial_init())
        pal_log(
            "RS-232C host bridge disabled (set X68K_RS232_PTY=1 or X68K_RS232_DEV=<path>)\n");

    x68k_bus_t *bus = x68k_bus_create();
    if (!bus || (cfg.local_memory_mb && cfg.cpu_model != MC68K_MODEL_68060) ||
        !x68k_bus_set_local_memory(bus, cfg.local_memory_mb)) {
        if (bus)
            x68k_bus_destroy(bus);
        pal_show_error("ix68k",
                       "local_memory は68060構成で off/0/16/32/64/128 を指定してください。"
                       "メモリ確保失敗時も起動できません。");
        pal_quit();
        return 1;
    }
    s_app.bus = bus;
    s_app.boot_time_us = x68k_bus_guest_mono_us(bus);
    x68k_bus_set_cpu_hz(bus, s_cpu_clock_hz);
    if (s_cpu_clock_hz != 10000000u)
        pal_log("CPU clock: %u MHz\n", s_cpu_clock_hz / 1000000u);
    x68k_bus_set_raster_y_live(bus, s_raster_y_live);
    x68k_bus_set_raster_pal_live(bus, s_raster_pal_live);
    if (!app_runtime_raster_sync_enabled())
        pal_log("Raster-sync rendering: off (frame-end batch compose, legacy behavior)\n");
    if (s_raster_y_live)
        pal_log("Raster-y: live (vertical scroll latch disabled)\n");
    if (s_raster_pal_live)
        pal_log("Raster-pal: live (palette latch disabled)\n");
    const int fpu_cir_enabled = x68k_fpu_uses_cir(cfg.cpu_model, cfg.fpu);
    if (!x68k_bus_set_fpu_cir(bus, fpu_cir_enabled,
                              x68k_fpu_uses_cir_secondary(cfg.fpu) ? X68K_FPU_CIR_SECONDARY
                                                                   : X68K_FPU_CIR_PRIMARY)) {
        pal_log("Error: FPU-CIR (MC68881) の生成に失敗しました\n");
        x68k_bus_destroy(bus);
        pal_quit();
        return 1;
    }
    if (fpu_cir_enabled)
        pal_log("FPU-CIR: on (%s)\n",
                x68k_fpu_uses_cir_secondary(cfg.fpu) ? "secondary" : "primary");
    x68k_bus_set_mfp_soft_eoi(bus, s_mfp_soft_eoi_enabled);
    if (s_mfp_soft_eoi_enabled)
        pal_log("MFP soft-EOI: on (VR S-bit / ISR in-service gating enabled)\n");
    x68k_bus_set_mfp_gpip_raster(bus, s_mfp_gpip_raster_enabled);
    if (!s_mfp_gpip_raster_enabled)
        pal_log("MFP GPIP raster model: off (bit7/bit4 use read-phase approximation)\n");
    pal_key_set_callbacks(mfp_usart_rx_push_cb, mfp_usart_rx_clear_cb, bus);
    pal_mouse_set_callbacks(scc_mouse_position_cb, scc_mouse_button_cb, bus);
    x68k_bus_scc_serial_set_carrier(bus, pal_serial_available()); /* DCD 相当 */

    if (cfg.rom_path[0]) {
        if (x68k_bus_load_iocs_rom(bus, cfg.rom_path) < 0) {
            pal_show_error("ix68k", "IPLROM の読み込みに失敗しました。");
            x68k_bus_destroy(bus);
            pal_quit();
            return 2;
        }
        pal_log("ROM loaded: %s\n", cfg.rom_path);
    }

    if (cfg.cgrom_path[0]) {
        if (x68k_bus_load_cgrom(bus, cfg.cgrom_path) < 0) {
            char msg[2048];
            x68k_cgrom_error_format(cfg.cgrom_path, msg, sizeof(msg));
            pal_show_error("ix68k", msg);
            x68k_bus_destroy(bus);
            pal_quit();
            return 2;
        }
        pal_log("CGROM loaded: %s\n", cfg.cgrom_path);
    }
    /* Linux版メニュー（File メニューと Window Size
     * サブメニュー、platform_linux_aux.c）の
     * 文字描画用。macOS/Windows（ネイティブメニュー）・ヘッドレス版では
     * 内部で無視される。 */
    pal_set_menu_cgrom_ank(x68k_bus_get_cgrom_ank(bus));

    if (cfg.hostdrv_dir[0]) {
        x68k_bus_hostdrv_configure(bus, cfg.hostdrv_dir);
        pal_log("HOSTDRV: %s を公開します\n", cfg.hostdrv_dir);
    }

    /* FDD: fd2/fd3 は external_fdd = true のときのみ有効 */
    if (!cfg.external_fdd && (cfg.fd_path[2][0] || cfg.fd_path[3][0])) {
        pal_log("Error: fd2/fd3 requires external_fdd = true\n");
        x68k_bus_destroy(bus);
        pal_quit();
        return 1;
    }
    /* FDD: XDF イメージをロード（設定されているドライブのみ） */
    for (int i = 0; i < 4; i++) {
        if (!cfg.fd_path[i][0])
            continue;
        if (x68k_bus_load_fdc(bus, i, cfg.fd_path[i]) == 0)
            pal_log("FD%d loaded: %s\n", i, cfg.fd_path[i]);
        else
            pal_log("FD%d load failed: %s\n", i, cfg.fd_path[i]);
    }
    if (cfg.external_fdd)
        x68k_bus_fdc_set_num_drives(bus, 4);

    /* SCSI ID 0-6 を接続する。"MO"/"MODRV" は空のリムーバブルドライブ、
     * "CD" は空のCD-ROMドライブを表す。 */
    for (int id = 0; id < 7; id++) {
        if (!cfg.scsi_disk_path[id][0])
            continue;
        if (strcasecmp(cfg.scsi_disk_path[id], "MO") == 0 ||
            strcasecmp(cfg.scsi_disk_path[id], "MODRV") == 0) {
            if (x68k_bus_scsi_insert_empty_removable(bus, id) == 0)
                pal_log("SCSI%d: 空のMOドライブとして接続しました\n", id);
            else
                pal_log("SCSI%d: 空MOドライブの接続に失敗しました\n", id);
        } else if (strcasecmp(cfg.scsi_disk_path[id], "CD") == 0) {
            if (x68k_bus_scsi_insert_empty_cdrom(bus, id) == 0)
                pal_log("SCSI%d: 空のCD-ROMドライブとして接続しました\n", id);
            else
                pal_log("SCSI%d: 空CD-ROMドライブの接続に失敗しました\n", id);
        } else if (x68k_bus_scsi_insert(bus, id, cfg.scsi_disk_path[id]) == 0) {
            pal_log("SCSI%d loaded: %s\n", id, cfg.scsi_disk_path[id]);
            if (cfg.scsi_overlay_path[id][0]) {
                if (x68k_bus_scsi_enable_overlay(bus, id, cfg.scsi_overlay_path[id]) == 0)
                    pal_log("SCSI%d overlay: %s\n", id, cfg.scsi_overlay_path[id]);
                else
                    pal_log("SCSI%d overlay load failed: %s\n", id, cfg.scsi_overlay_path[id]);
            }
        } else {
            pal_log("SCSI%d load failed: %s\n", id, cfg.scsi_disk_path[id]);
        }
    }

    /* SRAM 永続化: ファイルがあればロード、なければ ROM デフォルト値で初期化 */
    if (x68k_bus_load_sram(bus, cfg.sram_path) == 0) {
        pal_log("SRAM loaded: %s\n", cfg.sram_path);
    } else {
        x68k_bus_init_sram_defaults(bus);
        pal_log("SRAM: initialized from ROM defaults (first run)\n");
    }

    /* -boot fdd|scsi: SRAM $ED0018(最優先ブートデバイス)をCLIから上書きする。
     * scsi_disk0/fd0等の接続設定とは独立した
     * フラグなので、実際に起動できるかは接続側の設定と合わせて指定すること。 */
    if (strcmp(cfg.boot_priority, "scsi") == 0) {
        x68k_bus_write_byte(bus, 0xED0018, 0xA0);
        x68k_bus_write_byte(bus, 0xED0019, 0x00);
        pal_log("-boot scsi: SRAM $ED0018=$A000 (SCSI優先起動)\n");
    } else if (strcmp(cfg.boot_priority, "fdd") == 0) {
        x68k_bus_write_byte(bus, 0xED0018, 0x00);
        x68k_bus_write_byte(bus, 0xED0019, 0x00);
        pal_log("-boot fdd: SRAM $ED0018=$0000 (FDD優先起動)\n");
    } else if (cfg.boot_priority[0]) {
        pal_log("-boot: 不明な値 '%s' (fdd または scsi を指定)\n", cfg.boot_priority);
    }

    mc68k_bus_t hooks = x68k_bus_get_hooks(bus);
    mc68k_state_t *cpu = mc68k_create_model_with_fpu(&hooks, cfg.cpu_model, fpu_config);
    if (!cpu) {
        pal_log("Error: CPU/FPCP の生成に失敗しました\n");
        x68k_bus_destroy(bus);
        pal_quit();
        return 1;
    }
    s_app.cpu = cpu;
    checkpoint_init(&(checkpoint_env_t){.serial_active = pal_serial_available,
                                        .auto_input_pending = checkpoint_auto_input_pending,
                                        .fd0_override_active = app_force_fd0_boot_active,
                                        .host_cleanup = checkpoint_host_cleanup});
    x68k_bus_set_cpu(bus, cpu);
    if (!x68k_bus_set_timing_profile(bus, cfg.timing_profile)) {
        x68k_bus_destroy(bus);
        pal_quit();
        return 1;
    }
    /* pull型IRQ受付。命令境界ごとにx68k_bus_irq_level()が
     * NMI/IOI/DMAC/SCC/MFPの現在の優先度を問い合わせる。古いIRQ線を
     * ポーリング粒度で受理してしまう窓を構造的になくす。 */
    mc68k_set_irq_level_hook(cpu, x68k_bus_irq_level, bus);
    /* reset がウォッチポイントの初期値を読む前に非侵襲 peek を登録する。 */
    mc68k_set_peek_hooks(cpu, x68k_peek8_thunk, x68k_peek16_thunk, x68k_peek32_thunk, bus);
    /* 既定のログは pal_log() へ渡し、リングダンプ中だけキャプチャする。 */
    dbg_init_log_handler();
    mc68k_reset(cpu);
    /* MFPレジスタ アクセス時にCPU実行中の消費済みサイクルまでタイマを進める。 */
    x68k_bus_mfp_set_cycle_source(bus, mfp_cycle_source, cpu);
    x68k_bus_dmac_set_cycle_source(bus, dmac_cycle_source, cpu);
    mc68k_set_trap_handler(cpu, iocs_handler, bus);
    /* PC履歴リングバッファ(PCRING)は既定オフ。-pcring true または MCP DIAG PCRING ON
     * で有効化。診断ログを有効化する経路とは独立して明示的に停止できる。 */
    if (cfg.pcring) {
        mc68k_set_pc_ring(cpu, 1);
        pal_log("-pcring: CPU PC-history ring enabled at startup\n");
    }
    /* レジスタ履歴リングバッファ(REGRING)は既定オフ。-regring true または MCP DIAG REGRING ON
     * で有効化。*/
    if (cfg.regring) {
        mc68k_set_reg_ring(cpu, 1);
        pal_log("-regring: CPU register-history ring enabled at startup\n");
    }
    /* PCTRACE1024は既定オフ。-pctrace1024 true または MCP DIAG PCTRACE1024 ON で有効化。*/
    if (cfg.pctrace1024) {
        mc68k_set_pctrace1024(cpu, 1);
        pal_log("-pctrace1024: 1024命令ごとのPC周期トレースを有効化\n");
    }
    s_app.text_auto_probe = cfg.text_auto_probe;
    /* 白帯検出HALTは既定オフ。X68K_WHITEBAND=1 または MCP WHITEBAND ON で有効化。*/
    if (getenv("X68K_WHITEBAND")) {
        s_app.whiteband_halt_on = 1;
        mc68k_set_pc_ring(cpu, 1);
        pal_log("X68K_WHITEBAND: _ABORTRST($FD) 検出で CPU を HALT する\n");
    }
    /* IOCSフルトレースは既定オフ。X68K_TRACE_IOCS=1で起動直後(リセット前)から有効化する
     * （MCP DIAG IOCS ONだとreset()呼び出しとの往復遅延の間に起動処理が終わってしまい、
     * ブート直後のROM処理を追えないため、起動時点で有効化する手段が必要）。 */
    if (getenv("X68K_TRACE_IOCS")) {
        s_app.trace_iocs = 1;
        pal_log("X68K_TRACE_IOCS: IOCS full trace enabled at startup\n");
    }
    /* ユーザ/スーパーバイザメモリ保護は互換性のため既定オフとする。 */
    if (getenv("X68K_SUPER_PROTECT")) {
        x68k_bus_set_super_protect(bus, 1);
        pal_log("X68K_SUPER_PROTECT: ユーザ/スーパーバイザ メモリ保護を有効化\n");
    }
    mcp_init();

    if (cfg.ori_illegal) {
        mc68k_set_opcode_illegal(cpu, 0x0000);
        pal_log("ori_illegal: ORI.B #$00,D0 (0x0000) -> ILLEGAL exception\n");
    }

    if (cfg.badmovea_log) {
        mc68k_set_badmovea_log(cpu, 1);
        pal_log("badmovea_log: BADMOVEA 不正ポインタ診断ログを有効化\n");
    }

    /* BADADDRはword/longアクセスごとに出力されうるX68000固有の24-bit診断。
     * PCリングをダンプしないため、BADJUMP/BADORIと異なりPCRINGは有効化しない。 */
    if (cfg.badaddr_log) {
        mc68k_set_badaddr_log(cpu, 1);
        pal_log("badaddr_log: BADADDR 24-bitアドレス診断ログを有効化\n");
    }

    if (cfg.badjump_log) {
        mc68k_set_badjump_log(cpu, 1);
        mc68k_set_pc_ring(cpu, 1);
        pal_log("badjump_log: BADJUMP 不正ジャンプ診断ログを有効化\n");
    }

    if (cfg.badori_log) {
        mc68k_set_badori_log(cpu, 1);
        mc68k_set_pc_ring(cpu, 1);
        pal_log("badori_log: BADORI (opcode 0x0000) 診断ログを有効化\n");
    }

    {
        mc68k_regs_t regs;
        mc68k_get_regs(cpu, &regs);
        pal_log("Reset: PC=%08X  SSP=%08X\n", regs.pc, regs.ssp);
    }

    s_app.audio.cycle_clock = (frame_cycle_clock_t){0};
    s_app.audio.opm_clock = (frame_cycle_clock_t){0};
    frame_cycle_clock_t pacing_clock = {0};
    frame_cycle_clock_set_period(&s_app.audio.cycle_clock, s_cpu_clock_hz, 1, 60);
    frame_cycle_clock_set_period(&s_app.audio.opm_clock, OPM_GEN_RATE_HZ, 1, 60);
    frame_cycle_clock_set_period(&pacing_clock, 1000000u, 1, 60);
    uint64_t next_frame = pal_get_ticks_us() + frame_cycle_clock_next(&pacing_clock);
    uint64_t frameskip_log_at = next_frame + 1000000ULL;
    unsigned frameskip_count = 0;
    unsigned hard_overload_count = 0;
    app_runtime_set_render_enabled(1);
    app_type_start(cfg.type_string, cfg.type_delay_sec);

    audio_resample_mode_t resample_mode = x68k_config_parse_resample_mode(cfg.resample_mode);
    int32_t *adpcm_fifo = NULL; /* 44100Hzリサンプル済み出力の待ち行列(ステレオ) */
    size_t adpcm_fifo_capacity = 0;

    /* 補助ウィンドウは必要になったときだけ生成する。 */
    pal_aux_window_t *disk_win = NULL;
    pal_aux_window_t *keyboard_win = NULL;

    /* マイクキャプチャデバイスの現在の起動状態（OSのマイクインジケータ制御用）。
     * デバイス自体まだopenしていないため0で始める。 */
    int mic_capture_active = 0;

    while (!pal_should_quit() && !x68k_bus_power_off_requested(bus)) {
        /* 性能・音声受入試験用。期限到達時も通常の終了経路へ進むため、
         * audio statsとSRAM保存を確実に取得できる。0（既定）は無期限。 */
        if (cfg.run_for_sec > 0 &&
            pal_get_ticks_us() - s_boot_time_us >= (uint64_t)cfg.run_for_sec * 1000000ULL)
            break;
        pal_poll_events();
        type_pump(); /* -type 自動入力（メインループでも供給） */

        pal_aux_hotkey_t hotkey = pal_aux_window_take_hotkey();
        switch (hotkey) {
        case PAL_AUX_HOTKEY_DISK_WINDOW:
            if (disk_win) {
                pal_aux_window_destroy(disk_win);
                disk_win = NULL;
            } else {
                int scsi_visible_ids[SCSI_WIN_NUM_UNITS];
                int num_scsi_visible = collect_visible_scsi_ids(bus, scsi_visible_ids);
                int h = disk_window_height(x68k_bus_fdc_num_drives(bus), num_scsi_visible);
                disk_win = pal_aux_window_create("Disk Selector", DISK_WIN_W, h);
            }
            break;
        case PAL_AUX_HOTKEY_KEYBOARD_WINDOW:
            if (keyboard_win) {
                pal_aux_window_destroy(keyboard_win);
                keyboard_win = NULL;
            } else {
                keyboard_win = kb_window_create();
            }
            break;
        case PAL_AUX_HOTKEY_RESET:
            /* 一時的な起動デバイス設定を復元してから通常リセットする。 */
            app_force_fd0_boot_cancel(bus);
            app_reset_machine(bus);
            break;
        case PAL_AUX_HOTKEY_RESET_FD0:
            /* 実機の OPT.1 押下リセットと同様、FD0 からの起動を要求する。 */
            app_reset_machine(bus);
            app_force_fd0_boot_begin(bus);
            break;
        case PAL_AUX_HOTKEY_ZOOM_1:
        case PAL_AUX_HOTKEY_ZOOM_1_5:
        case PAL_AUX_HOTKEY_ZOOM_2: {
            /* 表示倍率は実機に無い設定なので SRAM には保存しない。 */
            int zoom_x10 = kWindowZoomX10[hotkey - PAL_AUX_HOTKEY_ZOOM_1];
            s_window_zoom = (float)zoom_x10 / WINDOW_ZOOM_X10_SCALE;
            pal_aux_set_current_window_zoom_x10(zoom_x10); /* メニューのチェックマーク更新 */
            par_corrected(s_app.native_w, s_native_dot_units, &display_w, &display_h);
            apply_window_zoom(&display_w, &display_h, s_window_zoom);
            pal_resize(s_app.native_w, s_app.native_h, display_w, display_h);
            pal_log("Window zoom: x%.1f\n", (double)s_window_zoom);
            break;
        }
        case PAL_AUX_HOTKEY_FULLSCREEN_TOGGLE:
            pal_toggle_fullscreen();
            break;
        case PAL_AUX_HOTKEY_NONE:
        default:
            break;
        }

        disk_win = update_disk_window(bus, disk_win);
        keyboard_win = update_keyboard_window(bus, keyboard_win);

        /* ジョイスティックはレベル状態なので毎フレーム読み取る。
         * 状態なので、イベントコールバックでなく毎フレームポーリングする。
         * マイク入力と同じ理由でhalt中も継続（PPIポートを読むCPU自体が
         * 止まっているため無害）。 */
        x68k_bus_set_joystick(bus, 0, pal_joystick_read(0));
        x68k_bus_set_joystick(bus, 1, pal_joystick_read(1));

        /* マイク入力はデバッグ停止中も吸い出し、SDL のキュー蓄積を防ぐ。
         * 毎フレーム実行する（SDL側のcaptureキューはハードウェアが実時間で
         * 記録し続けるため、halt中に吸い出しを止めるとSDL側で無制限に
         * 溜まってしまう。halt中にpushしても$E92003を読むCPU自体が
         * 止まっているため無害）。
         *
         * ゲストが実際に録音中（REC ST書き込み済み）の間だけキャプチャ
         * デバイスをopenする。pausedで開いておくだけでもOSのマイク使用
         * インジケータが点灯し続けてしまうため、録音していない間は
         * デバイス自体をcloseする（プライバシー上望ましい挙動）。 */
        int mic_should_be_active = x68k_bus_adpcm_is_recording(bus);
        if (mic_should_be_active != mic_capture_active) {
            if (mic_should_be_active) {
                if (!pal_audio_capture_init((int)AUDIO_CAPTURE_RATE_HZ, AUDIO_CAPTURE_CHANNELS))
                    pal_log(
                        "Audio capture (microphone) unavailable - recording will be silent\n");
                else
                    pal_audio_capture_set_active(1);
            } else {
                pal_audio_capture_quit();
            }
            mic_capture_active = mic_should_be_active;
        }
        if (pal_audio_capture_available()) {
            int16_t mic_raw[AUDIO_CAPTURE_BUF_SAMPLES];
            int got = pal_audio_capture_dequeue(mic_raw, AUDIO_CAPTURE_BUF_SAMPLES);
            if (got > 0) {
                uint32_t native_rate_hz = x68k_bus_adpcm_sample_rate_hz(bus);
                int16_t mic_native[AUDIO_REC_BUF_SAMPLES];
                int n = audio_resample_mono(mic_raw, got, AUDIO_CAPTURE_RATE_HZ, native_rate_hz,
                                            mic_native, AUDIO_REC_BUF_SAMPLES, resample_mode,
                                            &s_app.audio.capture_resample_state);
                x68k_bus_adpcm_push_rec_samples(bus, mic_native, n);
            }
        }

        /* RS-232C(SCC ch A)をフレーム単位でホストと橋渡しする。ホスト→
         * ゲストはリングバッファへ積むだけ、ゲスト→ホストはリングから
         * 取り出してそのまま書き出す。 */
        if (pal_serial_available()) {
            uint8_t serial_rx[64];
            int got = pal_serial_read(serial_rx, (int)sizeof(serial_rx));
            if (got > 0)
                x68k_bus_scc_recv_a(bus, serial_rx, got);

            uint8_t serial_tx[64];
            int to_send = x68k_bus_scc_send_a_dequeue(bus, serial_tx, (int)sizeof(serial_tx));
            if (to_send > 0)
                pal_serial_write(serial_tx, to_send);
        }

        /* CPU 実行と MFP タイマを MFP_TICK_CYCLES 単位で交互に進める。
         * FDC/SPC (IRQ1/IOI)・DMAC ch2/ch3 (IRQ3、IOCS _DMAMOVE系/_ADPCMOUT等用)・
         * SCC ch A/B (IRQ5)・MFP (IRQ6) の要求は、毎チャンク後の x68k_cycle_catchup()
         * で周辺を進めたうえで、CPUが命令境界ごとに x68k_bus_irq_level()（pull型）
         * で問い合わせて受け付ける（優先度は NMI→1→3→5→6）。
         * DMAC ch3(ADPCM)もMSM6258のサンプリング周波数に同期した実時間転送
         * なので、MFP/OPMと同じチャンク粒度で進める。フレーム単位では
         * CCR の一時停止と再開の短い間隔を表現できず、音切れが生じる。 */
        /* CRTC R20 由来の解像度をフレーム先頭で反映する。
         * 合成途中で寸法が変わらないよう、直前フレーム末尾の状態を使う。 */
        {
            int new_w, new_h;
            x68k_bus_get_display_size(bus, &new_w, &new_h);
            int dot_units = x68k_bus_get_dot_width_units(bus);
            if (new_w != s_app.native_w || new_h != s_app.native_h ||
                dot_units != s_native_dot_units) {
                s_app.native_w = new_w;
                s_app.native_h = new_h;
                s_native_dot_units = dot_units;
                par_corrected(s_app.native_w, s_native_dot_units, &display_w, &display_h);
                apply_window_zoom(&display_w, &display_h, s_window_zoom);
                pal_resize(s_app.native_w, s_app.native_h, display_w, display_h);
            }
        }

        x68k_crtc_frame_timing_t crtc_timing;
        x68k_bus_crtc_get_frame_timing(bus, &crtc_timing);
        frame_cycle_clock_set_period(&s_app.audio.cycle_clock, s_cpu_clock_hz,
                                     crtc_timing.numerator, crtc_timing.denominator);
        frame_cycle_clock_set_period(&s_app.audio.opm_clock, OPM_GEN_RATE_HZ,
                                     crtc_timing.numerator, crtc_timing.denominator);
        frame_cycle_clock_set_period(&pacing_clock, 1000000u, crtc_timing.numerator,
                                     crtc_timing.denominator);
        uint64_t frame_us = frame_cycle_clock_next(&pacing_clock);

        if (!dbg_is_paused()) { /* デバッガ停止中（BP/WP/例外/白帯検出/DBG_BREAK）は
                                 * CPUを進めず状態を保全（描画・MCPは継続） */
            /* OPM の目標サンプル数を先に確定し、CPU チャンクごとに生成する。
             * フレーム先頭で一括生成すると、そのフレーム中に
             * 音楽ドライバが行うOPMレジスタ書き込み（ソフトLFO/エンベロープ等、
             * テンポ割り込み毎の高頻度書き込み）が次フレームまで音に反映
             * されず60Hzに量子化され、ジッタ性の歪み(ザラザラ/ガリガリ)に
             * なるため、CPU 進行に比例した分だけ生成する。 */
            int n62 = (int)frame_cycle_clock_next(&s_app.audio.opm_clock);
            size_t previous_audio_frames = s_app.audio_buffers.frames;
            int audio_ready =
                audio_frame_buffers_reserve(&s_app.audio_buffers, (size_t)n62 + OPM_LOOKAHEAD);
            if (!audio_ready)
                pal_log("audio buffer allocation failed; output muted for one frame\n");
            if (audio_ready) {
                adpcm_fifo = s_app.audio_buffers.adpcm_fifo;
                adpcm_fifo_capacity = s_app.audio_buffers.fifo_frames;
                if (s_app.audio_buffers.frames != previous_audio_frames)
                    s_app.audio.adpcm_fifo_len = 0;
            }
            int32_t *opm_raw = s_app.audio_buffers.opm_raw;
            /* 前フレーム末尾で先読み生成済みの分をバッファ先頭へ引き継ぐ
             * （チャンネル順そのままinterleaved L,R）。 */
            if (audio_ready)
                memcpy(opm_raw, s_app.audio.opm_carry,
                       sizeof(int32_t) * (size_t)(s_app.audio.opm_carry_len * 2));
            else
                s_app.audio.opm_carry_len = 0;
            int opm_written = s_app.audio.opm_carry_len;

            const int cycles_per_frame = (int)frame_cycle_clock_next(&s_app.audio.cycle_clock);
            /* 現フレームの走査は、先頭で確定した表示高さを基準にする。 */
            x68k_bus_crtc_raster_frame_start(bus, cycles_per_frame, s_app.native_h);
            int remaining = cycles_per_frame;
            int cycles_done = 0;
            while (remaining > 0) {
                int chunk = remaining < MFP_TICK_CYCLES ? remaining : MFP_TICK_CYCLES;
                /* MFPタイマ満了の直後の命令境界でIRQ線が見えるよう、
                 * 満了予定でチャンクを切り詰める（満了検出が最大1チャンク遅れない）。 */
                int to_mfp_event = x68k_bus_mfp_cycles_to_next_event(bus);
                if (to_mfp_event < chunk)
                    chunk = to_mfp_event;
                int to_dmac_event = x68k_bus_dmac_cycles_to_next_event(bus);
                if (to_dmac_event < chunk)
                    chunk = to_dmac_event;
                x68k_bus_mfp_chunk_begin(bus);
                x68k_bus_dmac_chunk_begin(bus);
                /* 自動実行中は dbg_exec() が通常の mc68k_execute() へ委譲する。 */
                int ran = dbg_exec(cpu, chunk);
                /* STOP/HALT 中は ran == 0 になるが、CPU が命令を進めなくても
                 * MFP タイマ・ラスターは実時間で進み続けなければならない。
                 * STOP を hardclock (MFP Timer C) で解除する OS（NetBSD/x68k）は、
                 * ここで周辺時間を止めると割り込み源ごと凍って二度と目覚めない。
                 * CPU 待機中のチャンクはチャンク全体を経過時間として周辺へ渡す
                 * （1サイクルずつ進めると1フレームあたり数十万回空回りする）。
                 * デバッガ停止など CPU_NORMAL のまま 0 が返る場合は従来どおり
                 * 1 サイクルだけ進める。 */
                int consumed = ran;
                if (consumed <= 0)
                    consumed = cpu_is_waiting(cpu) ? chunk : 1;
                x68k_cycle_catchup(bus, consumed);
                mcp_perf_account(ran, consumed);
                remaining -= consumed;
                cycles_done += consumed;

                /* このチャンクまでに経過したCPUサイクルの割合分だけOPMを
                 * 生成する（端数はフレーム末尾でまとめて生成）。 */
                int target = (int)((int64_t)cycles_done * n62 / cycles_per_frame);
                if (target > n62)
                    target = n62;
                if (target > opm_written) {
                    if (audio_ready)
                        x68k_bus_opm_generate(bus, opm_raw + opm_written * 2,
                                              target - opm_written);
                    else
                        audio_discard_opm(bus, target - opm_written);
                    opm_written = target;
                }

                if (dbg_after_chunk(bus))
                    break; /* デバッガの停止（BP/WP/例外/DBG_BREAK要求、および
                            * IOCSハンドラ内で検出した白帯）で即停止 */
            }
            {
                /* フレーム末尾で残りのラスタを一気に消化する（CPU停止中もラスターが
                 * 進み続けるようにする保険）。
                 * 通常フレームではadvance()がMFP_TICK_CYCLES(約200)粒度で毎回
                 * 呼ばれるため（1ラスタ約293サイクル）、ここで残るのは丸め誤差の
                 * 高々1〜2本。デバッガ停止による早期break時は数百本残ることがあるが、
                 * その場合は下のフォールバックでx68k_compose_frame()が
                 * フレーム全体を描き直すため、64本を超えた分の取りこぼしは
                 * 実害が無い。 */
                int raster_lines[64];
                int n = x68k_bus_crtc_raster_flush(bus, raster_lines, 64);
                if (app_runtime_raster_sync_enabled() && app_runtime_render_enabled()) {
                    int cap = n < 64 ? n : 64;
                    for (int li = 0; li < cap; li++) {
                        int row = x68k_bus_crtc_raster_display_row(bus, raster_lines[li]);
                        if (row >= 0)
                            x68k_compose_line(bus, s_app.framebuf, s_app.native_w,
                                              s_app.native_h, row);
                    }
                }
            }
            if (opm_written < n62) { /* 白帯早期終了・端数分の生成漏れを補う */
                if (audio_ready)
                    x68k_bus_opm_generate(bus, opm_raw + opm_written * 2, n62 - opm_written);
                else
                    audio_discard_opm(bus, n62 - opm_written);
                opm_written = n62;
            }
            /* 次フレームのリサンプル先読み用に、n62の続きをOPM_LOOKAHEAD分
             * 追加生成して持ち越す（このフレームの出力(n62)には含めない）。 */
            if (audio_ready) {
                x68k_bus_opm_generate(bus, opm_raw + n62 * 2, OPM_LOOKAHEAD);
                memcpy(s_app.audio.opm_carry, opm_raw + n62 * 2,
                       sizeof(int32_t) * (OPM_LOOKAHEAD * 2));
                s_app.audio.opm_carry_len = OPM_LOOKAHEAD;
            } else {
                audio_discard_opm(bus, OPM_LOOKAHEAD);
            }

            if (!audio_ready) {
                /* OPM/ADPCMの過去サンプルとリサンプラ位相を捨て、無音を同じ概算時間だけ
                 * キューする。確保が回復したときに古い音を再生しない。 */
                audio_discard_adpcm(bus);
                s_app.audio.adpcm_in_carry_len = 0;
                s_app.audio.adpcm_fifo_len = 0;
                s_app.audio.adpcm_last_out[0] = 0;
                s_app.audio.adpcm_last_out[1] = 0;
                s_app.audio.opm_resample_state = (audio_resample_state_t){0};
                s_app.audio.playback_resample_state = (audio_resample_state_t){0};
                s_app.audio.adpcm_dc_state = (audio_dc_block_state_t){0};
                audio_queue_silence((n62 * (int)AUDIO_OUT_RATE_HZ + (int)OPM_GEN_RATE_HZ - 1) /
                                    (int)OPM_GEN_RATE_HZ);
            }

            /* V-DISP(ch6)/Timer A(ch13)は垂直映像期間終了時に発火する。
             * ラスターが有効なフレームでは垂直映像期間終了の瞬間に
             * crtc_raster_emit_line() 内部から発火済みなので、
             * ラスター無効時のみ、従来どおりフレーム末尾で発火させる二段構え。 */
            if (!x68k_bus_crtc_raster_valid(bus))
                x68k_bus_mfp_vdisp_tick(bus);
            {
                unsigned gfx_writes, text_writes;
                x68k_bus_take_raster_palette_writes(bus, &gfx_writes, &text_writes);
                if (gfx_writes || text_writes)
                    pal_log("Raster palette writes during V-DISP: gfx=%u text/sp=%u bytes\n",
                            gfx_writes, text_writes);
            }
            iocs_key_wait_frame_tick(); /* WAIT_KEYIN ポーリング検出の1フレーム集計 */
            /* _B_PUTMES 描画後のTVRAMを取り込む。呼ぶのはこのフレームのCPU実行を
             * 早期break無しに完走した場合のみ: 上の if (!dbg_is_paused()) は
             * ブロック入口でしか評価されないため、ループ内で dbg_after_chunk() が
             * pause を成立させて break したフレームでもここへ到達してしまい、
             * そのまま呼ぶと描画途中の内容をスナップショットしうる。 */
            if (remaining <= 0)
                x68k_bus_putmes_snapshot_tick(bus);

            /* OPM と ADPCM を出力レートへ変換して加算する。
             * OPMは上記でこのフレーム分(約1042サンプル)を生成済み、出力
             * レートへリサンプル。ADPCMはtick()で今フレーム分デコードされた
             * 出力リングを吸い出し、同じ出力レートへリサンプル。両者をint32
             * のまま加算し、最終段でのみ16bitクリップしてSDLへキューする
             * （中間クリップによる歪みを避ける）。pal_audio_available()==0
             * （ヘッドレス等）ならpal_audio_queue()がno-opなので分岐不要。 */
            if (audio_ready) {
                int32_t *opm_out = s_app.audio_buffers.opm_out;
                int no = audio_resample_stereo_i32(
                    opm_raw, n62, n62 + OPM_LOOKAHEAD, OPM_GEN_RATE_HZ, AUDIO_OUT_RATE_HZ,
                    opm_out, (int)s_app.audio_buffers.frames, resample_mode,
                    &s_app.audio.opm_resample_state);

                /* ADPCM の自然出力数は OPM 由来の要求数 `no` と数サンプルずれる。
                 * 出力を打ち切ってゼロで埋めるとフレーム境界で波形が不連続になるため、
                 * (1)入力を末尾ADPCM_LOOKAHEAD分だけ次フレームへ持ち越して補間の
                 *    先読みを確保(OPM経路と同じcarry方式)し、
                 * (2)自然数だけリサンプルして出力FIFOへ積み、
                 * (3)FIFOから正確に`no`フレーム取り出す
                 * ことで、count不一致を吸収しつつ波形を連続させる。 */
                int32_t *adpcm_out = s_app.audio_buffers.adpcm_out;
                {
                    int16_t *mono = s_app.audio_buffers.mono;
                    uint32_t native_rate_hz;
                    int left_on, right_on;
                    /* 前フレームの先読み持ち越し分をバッファ先頭に置く */
                    if (s_app.audio.adpcm_in_carry_len > 0)
                        memcpy(mono, s_app.audio.adpcm_in_carry,
                               sizeof(int16_t) * (size_t)s_app.audio.adpcm_in_carry_len);
                    int taken = x68k_bus_adpcm_take_samples(
                        bus, mono + s_app.audio.adpcm_in_carry_len,
                        (int)s_app.audio_buffers.frames - s_app.audio.adpcm_in_carry_len,
                        &native_rate_hz, &left_on, &right_on);
                    int in_total = s_app.audio.adpcm_in_carry_len + taken;

                    /* 末尾 ADPCM_LOOKAHEAD 分は確定せず次フレームへ持ち越す。
                     * in_count 分だけ位相を進め、in_valid に先読み分を
                     * 含めることで境界補間が末尾複製にならないようにする。 */
                    int commit = in_total - ADPCM_LOOKAHEAD;
                    if (commit > 0) {
                        int32_t *tmp = s_app.audio_buffers.adpcm_tmp;
                        int nat = audio_mix_resample_pan_i32(
                            mono, commit, in_total, native_rate_hz, AUDIO_OUT_RATE_HZ, left_on,
                            right_on, tmp, (int)s_app.audio_buffers.frames, resample_mode,
                            &s_app.audio.playback_resample_state);
                        /* FIFOへ追加(容量超過分は古いフレームを捨てる。通常は発生
                         * しないが起動時バースト等の保険)。 */
                        if ((size_t)nat > adpcm_fifo_capacity) {
                            /* 単フレームでFIFO容量を超える異常時は新しい方を優先 */
                            size_t keep = adpcm_fifo_capacity;
                            memcpy(adpcm_fifo, tmp + ((size_t)nat - keep) * 2,
                                   sizeof(int32_t) * 2 * keep);
                            s_app.audio.adpcm_fifo_len = (int)keep;
                        } else {
                            if ((size_t)(s_app.audio.adpcm_fifo_len + nat) >
                                adpcm_fifo_capacity) {
                                int drop =
                                    s_app.audio.adpcm_fifo_len + nat - (int)adpcm_fifo_capacity;
                                memmove(adpcm_fifo, adpcm_fifo + drop * 2,
                                        sizeof(int32_t) * 2 *
                                            (size_t)(s_app.audio.adpcm_fifo_len - drop));
                                s_app.audio.adpcm_fifo_len -= drop;
                            }
                            memcpy(adpcm_fifo + s_app.audio.adpcm_fifo_len * 2, tmp,
                                   sizeof(int32_t) * 2 * (size_t)nat);
                            s_app.audio.adpcm_fifo_len += nat;
                        }
                        /* 持ち越し入力を更新(常にmin(in_total,ADPCM_LOOKAHEAD)) */
                        s_app.audio.adpcm_in_carry_len = in_total - commit;
                        memmove(s_app.audio.adpcm_in_carry, mono + commit,
                                sizeof(int16_t) * (size_t)s_app.audio.adpcm_in_carry_len);
                    } else {
                        /* 確定できる入力がない場合は持ち越しを
                         * 積み増して次フレームへ。 */
                        s_app.audio.adpcm_in_carry_len = in_total;
                        if (in_total > 0)
                            memmove(s_app.audio.adpcm_in_carry, mono,
                                    sizeof(int16_t) * (size_t)in_total);
                    }

                    /* FIFOから`no`フレーム取り出す。枯渇時は直近値から
                     * ±1/サンプルずつ 0 へ減衰させながら埋める。
                     * 即座に0へ落とすと大きな段差でクリックになるため
                     * (XEiJ SoundSource.javaのゼロ減衰に倣う)。 */
                    int pop =
                        (no <= s_app.audio.adpcm_fifo_len) ? no : s_app.audio.adpcm_fifo_len;
                    if (pop > 0) {
                        memcpy(adpcm_out, adpcm_fifo, sizeof(int32_t) * 2 * (size_t)pop);
                        s_app.audio.adpcm_last_out[0] = adpcm_fifo[(pop - 1) * 2 + 0];
                        s_app.audio.adpcm_last_out[1] = adpcm_fifo[(pop - 1) * 2 + 1];
                        s_app.audio.adpcm_fifo_len -= pop;
                        memmove(adpcm_fifo, adpcm_fifo + pop * 2,
                                sizeof(int32_t) * 2 * (size_t)s_app.audio.adpcm_fifo_len);
                    }
                    for (int i = pop; i < no; i++) {
                        for (int ch = 0; ch < 2; ch++) {
                            int32_t v = s_app.audio.adpcm_last_out[ch];
                            if (v > 0)
                                v--;
                            else if (v < 0)
                                v++;
                            s_app.audio.adpcm_last_out[ch] = v;
                            adpcm_out[i * 2 + ch] = v;
                        }
                    }
                }

                if (no > 0) {
                    /* ADPCMコーデックのpredictorはリーク項の無い積分器で、
                     * 特定のサンプルがDC寄りにデコードされるとノート境界で
                     * ステップ状のDCオフセットとして持続し可聴クリックに
                     * なる。OPM は DC フリーなため ADPCM 側のみに
                     * ハイパスを掛けてから合成する。 */
                    audio_dc_block_stereo(adpcm_out, no, &s_app.audio.adpcm_dc_state);
                    int16_t *mixed = s_app.audio_buffers.mixed;
                    audio_mix_sum_clip_stereo(opm_out, adpcm_out, no, mixed);
                    pal_audio_queue(mixed, no);
                }
            }
        }

        if (mc68k_get_exec_state(cpu) == CPU_HALTED) {
            mc68k_regs_t regs;
            mc68k_get_regs(cpu, &regs);
            pal_log("CPU HALTED at PC=%08X\n", regs.pc);
        }

        /* ネイティブ解像度で描画し、SDL が表示時にストレッチする。
         * ラスタ同期が有効で走査状態が妥当なら、上の
         * チャンクループ内で各表示行がすでに x68k_compose_line() で確定済み。
         * 無効・OFF・白帯検出停止中のフレームだけ、ここで一括合成する
         * フォールバックを行う。 */
        int rendered_this_frame = app_runtime_render_enabled();
        if (rendered_this_frame) {
            if (dbg_is_paused() || !app_runtime_raster_sync_enabled() ||
                !x68k_bus_crtc_raster_valid(bus)) {
                x68k_compose_frame(bus, s_app.framebuf, s_app.native_w, s_app.native_h);
            }
            pal_present_frame(s_app.framebuf, s_app.native_w, s_app.native_h);
            /* 負荷再現専用。描画負荷だけを増やし、CPU/音声時系列は変えない。 */
            if (cfg.render_load_us > 0)
                pal_sleep_us((uint64_t)cfg.render_load_us);
        }
        /* MCPのDBG_STEPはx68k_cycle_catchup()を直接呼ぶ。自動skipの状態を
         * 持ち込まず、単独ステップでは従来どおり画面を合成できるようにする。 */
        app_runtime_set_render_enabled(1);
        mcp_tick(bus);

        /* hard overload再現専用。描画を省略しても残るホスト負荷を
         * 加えるため、render_enabledの内側には置かない。 */
        if (cfg.frame_load_us > 0)
            pal_sleep_us((uint64_t)cfg.frame_load_us);

        /* システムワーク$9CA(2バイト)を起動後経過時間(1/100秒、16bit wrap)で
         * 毎フレーム更新する。IOCS _ONTIME($7F)はTRAP呼び出し時にD0へ直接計算値を
         * 返すのみでRAM上のシステムワークは更新しないため、SUSIE.X等サードパーティ
         * ドライバが$9D6/$9CA/$9CC（_ONTIME内部実装が参照する生のワーク変数）を
         * TRAPを経由せず直接読むと常に0のまま進まず、経過時間待ちループが無限に
         * 終わらない。$9D6・$9CCは0のまま(未使用)にし、$9CAだけを
         * 生かすことで GetTime()相当の計算式(($9CA)-($9CC)+((($9D6)/1440%1440)*6000))
         * が実質($9CA)そのもの=起動後経過1/100秒(mod 65536)を返すようにする。
         * ただしVBR != 0（NetBSD等がIOCS/Human68k用の低位RAMを捨てて独自のベクタ
         * テーブルへ切り替えた後）は書かない。NetBSD/x68kのカーネルtextはVA 0から
         * 始まり$9CAはcpu_switchto内の命令に当たるため、書くと命令が壊れる。 */
        uint64_t now = pal_get_ticks_us();
        mc68k_regs_t ontime_regs;
        mc68k_get_regs(cpu, &ontime_regs);
        if (ontime_regs.vbr == 0) {
            uint64_t elapsed_us = x68k_bus_guest_mono_us(bus) - s_app.boot_time_us;
            uint16_t centisec = (uint16_t)((elapsed_us / 10000ULL) & 0xFFFFu);
            x68k_bus_write_byte(bus, 0x9CA, (uint8_t)(centisec >> 8));
            x68k_bus_write_byte(bus, 0x9CB, (uint8_t)centisec);
        }

        if (frame_timing_is_hard_overload(rendered_this_frame, now, next_frame))
            hard_overload_count++;
        int render_next = !dbg_is_paused() && (!frameskip_enabled || now <= next_frame);
        if (now < next_frame)
            pal_sleep_us(next_frame - now);
        next_frame += frame_us;
        if (now > next_frame + FRAME_CATCHUP_LIMIT_US) {
            next_frame = now + frame_us; /* 遅れすぎた分は取り戻さない */
            render_next = 1;
        }
        if (!render_next)
            frameskip_count++;
        if (now >= frameskip_log_at) {
            if (frameskip_count != 0)
                pal_log("frameskip: %u frame(s)/s (render load %dus)\n", frameskip_count,
                        cfg.render_load_us);
            if (hard_overload_count != 0)
                pal_log("hard-overload: %u frame(s)/s (CPU/peripheral/audio cannot keep CRTC "
                        "timing)\n",
                        hard_overload_count);
            frameskip_count = 0;
            hard_overload_count = 0;
            frameskip_log_at = now + 1000000ULL;
        }
        app_runtime_set_render_enabled(render_next);
    }

    mc68k_destroy(cpu);
    mcp_cleanup();
    audio_frame_buffers_destroy(&s_app.audio_buffers);

    {
        pal_audio_stats_t audio_stats;
        pal_audio_get_stats(&audio_stats);
        pal_log("audio stats: reset=%llu underflow=%llu queue-limit=%llu\n",
                (unsigned long long)audio_stats.reset_count,
                (unsigned long long)audio_stats.underflow_count,
                (unsigned long long)audio_stats.queue_limit_count);
    }

    /* SRAM 終了時保存 */
    if (x68k_bus_save_sram(bus, cfg.sram_path) == 0)
        pal_log("SRAM saved: %s\n", cfg.sram_path);
    else
        pal_log("SRAM save failed: %s\n", cfg.sram_path);

    x68k_bus_destroy(bus);
    pal_quit();
    return 0;
}
