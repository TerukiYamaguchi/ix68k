#pragma once
/*
 * app_internal.h — アプリ層（main.c / app/ / mcp/ / iocs.c）で共有する
 * 内部状態・関数宣言。公開 API ではなくアプリ層内部の結合専用。
 *
 * 所有権:
 *   - アプリ層をまたぐ状態は main.c が生成する s_app に集約する。
 *   - リセット・FD0強制起動・CPU後段同期・自動入力は app/runtime.c が定義。
 *   - IOCS トレース状態の更新は iocs.c、MCP サーバ関数は mcp/server.c が担当する。
 *     MCP コマンド（mcp/commands.c）も trace_iocs / whiteband_halt_on 等を設定する。
 */
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "app/frame_timing.h"
#include "audio/audio_mixer.h"
#include "bus/x68k_bus.h"
#include "mc68000/cpu_types.h"

/* 画面バッファ最大サイズ（s_app.framebuf の確保に使用） */
#define SCREEN_MAX_W 1024
#define SCREEN_MAX_H 1024

#define OPM_LOOKAHEAD   2
#define ADPCM_LOOKAHEAD 2

typedef struct {
    int32_t *opm_raw;
    int32_t *opm_out;
    int32_t *adpcm_out;
    int32_t *adpcm_tmp;
    int32_t *adpcm_fifo;
    int16_t *mono;
    int16_t *mixed;
    size_t frames;
    size_t fifo_frames;
} audio_frame_buffers_t;

typedef struct {
    frame_cycle_clock_t cycle_clock;
    frame_cycle_clock_t opm_clock;
    int32_t opm_carry[OPM_LOOKAHEAD * 2];
    int opm_carry_len;
    int16_t adpcm_in_carry[ADPCM_LOOKAHEAD];
    int adpcm_in_carry_len;
    int adpcm_fifo_len;
    int32_t adpcm_last_out[2];
    audio_dc_block_state_t adpcm_dc_state;
    audio_resample_state_t playback_resample_state;
    audio_resample_state_t opm_resample_state;
    audio_resample_state_t capture_resample_state;
} app_audio_state_t;

/* main.c が生成・破棄を担当する、アプリ層横断の実行状態。
 * iocs.c、MCP 実装、app/runtime.c は必要なメンバーだけを参照する。 */
typedef struct {
    mc68k_state_t *cpu;
    x68k_bus_t *bus;
    int native_w;
    int native_h;
    uint32_t framebuf[SCREEN_MAX_W * SCREEN_MAX_H];
    time_t boot_time;
    uint64_t boot_time_us; /* IOCS _ONTIME用: ゲスト単調時計基準の起動時刻(us) */
    app_audio_state_t audio;
    audio_frame_buffers_t audio_buffers;
    int trace_iocs;
    int text_auto_probe; /* MCP CONFIG GET TEXT_AUTO_PROBE が返す値（起動時設定） */
    int whiteband_halt_on;
    int iocs_ring_pos;
    int iocs_ring_cnt;
    volatile int key_input_waiting;
    /* MCP PASTEが文字を順次注入中なら1。途中のB_KEYINPを「次のコマンドを
     * 受け付けられる待機」と誤認しないため、commands.cが管理する。 */
    volatile int key_input_paste_active;
} x68k_app_t;

extern x68k_app_t s_app;

void type_pump(void);
void app_type_start(const char *text, int delay_sec);
int app_type_pending(void);
void app_runtime_set_raster_sync_enabled(int enabled);
int app_runtime_raster_sync_enabled(void);
/* 自動フレームスキップ中は0。CPU/周辺の進行は止めず、描画だけを省略する。
 * デバッガの単独ステップではmain.cが必ず1へ戻してから使う。 */
void app_runtime_set_render_enabled(int enabled);
int app_runtime_render_enabled(void);

/* CPU 実行後に周辺機器を同期し、割り込みを再評価する。
 * ran <= 0 でも CRTC は 1 サイクル進め、停止中に走査が凍結するのを防ぐ。 */
void x68k_cycle_catchup(x68k_bus_t *bus, int ran);

/* ROM ベクタ、周辺機器、CPU、デバッグ状態の順にマシン全体をリセットする。 */
void app_reset_machine(x68k_bus_t *bus);

/* $ED0018 を標準の探索順へ一時変更し、FD0 からの起動を要求する。
 * 呼び出し前に app_reset_machine() でリセットしておく。 */
void app_force_fd0_boot_begin(x68k_bus_t *bus);

/* FD0 強制起動を打ち切り、保存していた $ED0018 の値を復元する。 */
void app_force_fd0_boot_cancel(x68k_bus_t *bus);

/* ROM による $ED0018 の読み出しを検出したら、一時上書きを復元する。 */
void app_force_fd0_boot_finish_if_read(x68k_bus_t *bus);
int app_force_fd0_boot_active(void);

/* s_app の iocs_ring_* / key_input_waiting は iocs.c が更新する。
 * trace_iocs / whiteband_halt_on は mcp/commands.c（DIAG IOCS / WHITEBAND）と
 * main.c（起動時設定）が設定し、iocs.c が参照する（trace_iocs は _ABORTRST で iocs.c が解除）。
 */

/* main.c が TRAP #15 ハンドラとして登録する。 */
int iocs_handler(mc68k_state_t *cpu, int trap_num, void *ctx);

/* WAIT_KEYIN のポーリング状態をフレーム単位で集計する。 */
void iocs_key_wait_frame_tick(void);

/* MCPの待機pollから呼ぶ。IOCSベクタ先を直接実行するROM待機ループも検出する。 */
void iocs_key_wait_poll(x68k_bus_t *bus);

/* WAIT_KEYIN のポーリング状態をクリアする。 */
void iocs_key_wait_reset(void);

/* MCP入力をキューへ積む直前に呼ぶ。直前の B_KEYINP 待ち通知を
 * 解除し、同じフレームに残った旧ポーリング集計がそれを復活させないようにする。 */
void iocs_key_wait_input_queued(void);

/* ---- mcp/server.c 定義 ---- */
void mcp_init(void);
void mcp_cleanup(void);
void mcp_tick(x68k_bus_t *bus);
void mcp_output_append(uint8_t ch); /* B_PUTC / B_PRINT の出力キャプチャ */
