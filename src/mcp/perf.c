#include "mcp/perf.h"

#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include <string.h>

#define PERF_PROBE_START 0u /* mc68k_set_entry_probes() の id */
#define PERF_PROBE_END   1u
#define PERF_PROBE_COUNT 2u

typedef struct {
    mcp_perf_status_t status;
    mc68k_state_t *cpu;  /* プローブを登録したCPU（所有中のみ非NULL） */
    int release_pending; /* コールバックが INVALID を記録した。dbg_exec() 復帰後に解除する */
} perf_state_t;

static perf_state_t s_perf;

const char *mcp_perf_state_name(mcp_perf_state_t state) {
    switch (state) {
    case MCP_PERF_WAIT_START:
        return "WAIT_START";
    case MCP_PERF_RUNNING:
        return "RUNNING";
    case MCP_PERF_COMPLETE:
        return "COMPLETE";
    case MCP_PERF_INVALID:
        return "INVALID";
    case MCP_PERF_IDLE:
    default:
        return "IDLE";
    }
}

void mcp_perf_account(int cpu_cycles, int elapsed_cycles) {
    if (cpu_cycles > 0)
        s_perf.status.total.cpu += (uint64_t)cpu_cycles;
    if (elapsed_cycles > 0)
        s_perf.status.total.elapsed += (uint64_t)elapsed_cycles;
}

/* 命令境界のスナップショット。現行チャンク内の先行命令分（前命令のバスウェイト
 * 含む、現在のマーカー命令自身は含まない）を累積へ足す。 */
static mcp_perf_stamp_t perf_snapshot(const mc68k_state_t *cpu) {
    uint64_t in_chunk = (uint64_t)mc68k_executed_cycles(cpu);
    mcp_perf_stamp_t stamp = {
        .cpu = s_perf.status.total.cpu + in_chunk,
        .elapsed = s_perf.status.total.elapsed + in_chunk,
    };
    return stamp;
}

/* コールバック内では mc68k_clear_entry_probes() を呼べない（debug.h の契約）。 */
static void perf_hit(mc68k_state_t *cpu, unsigned id, uint32_t pc, void *ctx) {
    (void)pc;
    (void)ctx;
    mcp_perf_state_t state = s_perf.status.state;
    if (id == PERF_PROBE_START) {
        if (state == MCP_PERF_WAIT_START || state == MCP_PERF_RUNNING) {
            s_perf.status.start = perf_snapshot(cpu);
            s_perf.status.state = MCP_PERF_RUNNING;
        }
    } else if (id == PERF_PROBE_END) {
        if (state == MCP_PERF_RUNNING) {
            s_perf.status.end = perf_snapshot(cpu);
            s_perf.status.state = MCP_PERF_COMPLETE;
        } else if (state == MCP_PERF_WAIT_START) {
            s_perf.status.state = MCP_PERF_INVALID;
            s_perf.release_pending = 1;
        }
    }
}

static void perf_release(void) {
    if (s_perf.cpu)
        mc68k_clear_entry_probes(s_perf.cpu);
    s_perf.cpu = NULL;
    s_perf.release_pending = 0;
}

int mcp_perf_arm(mc68k_state_t *cpu, uint32_t start_pc, uint32_t end_pc) {
    if (!cpu || (start_pc & 1u) || (end_pc & 1u) || start_pc == end_pc)
        return MCP_PERF_ARM_BADARG;
    perf_release(); /* 前回の測定が残っていれば、自分のプローブを解除して置換する */
    const uint32_t pcs[PERF_PROBE_COUNT] = {start_pc, end_pc};
    if (mc68k_set_entry_probes(cpu, pcs, PERF_PROBE_COUNT, perf_hit, &s_perf) != 0)
        return MCP_PERF_ARM_BUSY;
    s_perf.cpu = cpu;
    s_perf.release_pending = 0;
    s_perf.status.sequence++;
    s_perf.status.state = MCP_PERF_WAIT_START;
    s_perf.status.start_pc = start_pc;
    s_perf.status.end_pc = end_pc;
    memset(&s_perf.status.start, 0, sizeof(s_perf.status.start));
    memset(&s_perf.status.end, 0, sizeof(s_perf.status.end));
    return 0;
}

void mcp_perf_clear(void) {
    perf_release();
    s_perf.status.state = MCP_PERF_IDLE;
}

void mcp_perf_invalidate(void) {
    mcp_perf_state_t state = s_perf.status.state;
    if (state != MCP_PERF_WAIT_START && state != MCP_PERF_RUNNING)
        return;
    perf_release();
    s_perf.status.state = MCP_PERF_INVALID;
}

void mcp_perf_after_exec(void) {
    if (s_perf.release_pending)
        perf_release();
}

void mcp_perf_reset(void) {
    mcp_perf_clear();
}

int mcp_perf_owns_probe(void) {
    return s_perf.cpu != NULL;
}

void mcp_perf_get_status(mcp_perf_status_t *out) {
    *out = s_perf.status;
}
