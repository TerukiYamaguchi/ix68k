#pragma once

#include <stdint.h>

#include "mc68000/cpu_types.h"

/*
 * 性能チューニング用サイクル計測。
 *
 * 累積カウンタは実行経路（通常実行・debug_run・DBG STEP）に依存せず進む。
 * 区間計測は開始・終了PCへのエントリプローブでラッチするだけで、
 * ブレークポイント・ウォッチポイント・debug_runは使わない。
 */

typedef enum {
    MCP_PERF_IDLE,
    MCP_PERF_WAIT_START,
    MCP_PERF_RUNNING,
    MCP_PERF_COMPLETE,
    MCP_PERF_INVALID,
} mcp_perf_state_t;

typedef struct {
    uint64_t cpu;     /* CPU実行サイクル（命令実行＋モデル化済みバスウェイト） */
    uint64_t elapsed; /* 仮想経過サイクル（STOP/HALT中に周辺だけを進めた分を含む） */
} mcp_perf_stamp_t;

typedef struct {
    mcp_perf_state_t state;
    uint64_t sequence;
    uint32_t start_pc;
    uint32_t end_pc;
    mcp_perf_stamp_t start;
    mcp_perf_stamp_t end;
    mcp_perf_stamp_t total; /* プロセス起動後の累積（リセットで戻さない） */
} mcp_perf_status_t;

/* 実行チャンク1回分を累積へ加算する。cpu_cycles は dbg_exec()/dbg_step() の戻り値
 * （0以下は0として扱う）、elapsed_cycles は周辺へ進めた consumed。 */
void mcp_perf_account(int cpu_cycles, int elapsed_cycles);

/* ARM結果。負値はエラー。 */
#define MCP_PERF_ARM_BADARG (-1) /* 奇数PC・開始と終了が同一・cpu NULL */
#define MCP_PERF_ARM_BUSY   (-2) /* コア側のプローブ設定に失敗 */

/* 偶数PC二つを登録し、前回ラッチを破棄して WAIT_START にする。成功時0。 */
int mcp_perf_arm(mc68k_state_t *cpu, uint32_t start_pc, uint32_t end_pc);

/* プローブを解除し IDLE に戻す（累積は保持）。 */
void mcp_perf_clear(void);

/* WAIT_START/RUNNING の測定を無効化（INVALID）してプローブを解除する。
 * debug_run 経路・DBG STEP・DBG BREAK の要求時に、コールバック外から呼ぶ。
 * COMPLETE/IDLE/INVALID では何もしない。 */
void mcp_perf_invalidate(void);

/* dbg_exec() の復帰直後に呼ぶ。コールバックが記録した解除保留を処理する。 */
void mcp_perf_after_exec(void);

/* 実行リセット用。測定状態を IDLE に戻しプローブを解除する（累積は保持）。 */
void mcp_perf_reset(void);

/* エントリプローブスロットを PERF が所有しているか（WAIT_START/RUNNING/COMPLETE）。 */
int mcp_perf_owns_probe(void);

void mcp_perf_get_status(mcp_perf_status_t *out);

const char *mcp_perf_state_name(mcp_perf_state_t state);
