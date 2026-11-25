#include "mcp/perf.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "memory/bus.h"
#include "memory/memory.h"

#define RAM_SIZE   0x10000u
#define PC         0x1000u
#define SSP        0x8000u
#define IRQ1_VEC   0x64u /* 自動ベクタ（レベル1） */
#define IRQ_HANDLER 0x2000u
#define OP_NOP     0x4E71u
#define OP_STOP    0x4E72u
#define OP_RTE     0x4E73u
#define SR_S_MASK0 0x2000u /* S=1, 割り込みマスク0 */
#define STOP_ITERS_BEFORE_IRQ 5
#define MAIN_CHUNK 100

static void put16(uint8_t *ram, uint32_t addr, uint16_t value) {
    ram[addr] = (uint8_t)(value >> 8);
    ram[addr + 1] = (uint8_t)value;
}
static void put32(uint8_t *ram, uint32_t addr, uint32_t value) {
    put16(ram, addr, (uint16_t)(value >> 16));
    put16(ram, addr + 2, (uint16_t)value);
}

static mc68k_state_t *make_cpu(uint8_t *ram) {
    mem_ctx_t *mem = mem_create();
    mem_region_t *region = calloc(1, sizeof(*region));
    assert(region);
    region->data = ram;
    region->size = RAM_SIZE;
    mem_add_region(mem, region);
    mc68k_bus_t bus = bus_make_hooks(mem);
    mc68k_state_t *cpu = mc68k_create(&bus);
    assert(cpu);
    return cpu;
}

/* PC から nops 個の NOP を並べる。 */
static mc68k_state_t *make_nop_cpu(uint8_t *ram, int nops) {
    memset(ram, 0, RAM_SIZE);
    put32(ram, 0, SSP);
    put32(ram, 4, PC);
    for (int i = 0; i < nops; i++)
        put16(ram, PC + (uint32_t)i * 2u, OP_NOP);
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    return cpu;
}

/* main.c のチャンクループと同じ会計規則（通常実行）。stop_iters は STOP 中の
 * 待機チャンク数を数える。 */
static int run_chunk(mc68k_state_t *cpu, int chunk, int *waiting_chunks) {
    int ran = mc68k_execute(cpu, chunk);
    mcp_perf_after_exec();
    int consumed = ran;
    if (consumed <= 0) {
        consumed = chunk; /* STOP/HALT 中は周辺へチャンク全体を渡す */
        if (waiting_chunks)
            (*waiting_chunks)++;
    }
    mcp_perf_account(ran, consumed);
    return ran;
}

static int nop_cycles(void) {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu = make_nop_cpu(ram, 4);
    int cycles = mc68k_execute(cpu, 1);
    assert(cycles > 0);
    mc68k_destroy(cpu);
    return cycles;
}

static mcp_perf_status_t run_nop_span(int chunk) {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu = make_nop_cpu(ram, 16);
    mcp_perf_clear();
    /* 開始 NOP は PC+4、終了 NOP は PC+10（間に開始を含む3命令）。 */
    assert(mcp_perf_arm(cpu, PC + 4, PC + 10) == 0);
    for (int i = 0; i < 16; i++)
        run_chunk(cpu, chunk, NULL);
    mcp_perf_status_t st;
    mcp_perf_get_status(&st);
    mcp_perf_clear();
    mc68k_destroy(cpu);
    return st;
}

/* 差分は開始 NOP を含み終了 NOP を含まない。チャンク分割・再実行で値が変わらない。 */
static void test_boundary_and_determinism(void) {
    const int nop = nop_cycles();
    const int chunks[] = {1, 4, 8, 100, 1000};
    mcp_perf_status_t first;
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        mcp_perf_status_t st = run_nop_span(chunks[i]);
        assert(st.state == MCP_PERF_COMPLETE);
        /* PC+4 から PC+10 まで: 命令 PC+4, PC+6, PC+8 の3つ。 */
        assert(st.end.cpu - st.start.cpu == (uint64_t)(3 * nop));
        assert(st.end.elapsed - st.start.elapsed == (uint64_t)(3 * nop));
        if (i == 0) {
            first = st;
            continue;
        }
        assert(st.end.cpu - st.start.cpu == first.end.cpu - first.start.cpu);
    }
}

/* PERF の有無は累積カウンタを変えない（非侵襲性）。 */
static void test_totals_independent_of_probe(void) {
    uint64_t total[2];
    for (int armed = 0; armed < 2; armed++) {
        uint8_t ram[RAM_SIZE];
        mc68k_state_t *cpu = make_nop_cpu(ram, 16);
        mcp_perf_clear();
        mcp_perf_status_t before;
        mcp_perf_get_status(&before);
        if (armed)
            assert(mcp_perf_arm(cpu, PC + 4, PC + 10) == 0);
        for (int i = 0; i < 16; i++)
            run_chunk(cpu, 4, NULL);
        mcp_perf_status_t after;
        mcp_perf_get_status(&after);
        total[armed] = after.total.cpu - before.total.cpu;
        mcp_perf_clear();
        mc68k_destroy(cpu);
    }
    assert(total[0] == total[1] && total[0] > 0);
}

/* STOP を含む区間では仮想経過が CPU 実行サイクルを上回る。 */
static void test_stop_elapsed_exceeds_cpu(void) {
    uint8_t ram[RAM_SIZE] = {0};
    put32(ram, 0, SSP);
    put32(ram, 4, PC);
    put32(ram, IRQ1_VEC, IRQ_HANDLER);
    put16(ram, PC, OP_NOP);     /* 開始 */
    put16(ram, PC + 2, OP_STOP);
    put16(ram, PC + 4, SR_S_MASK0);
    put16(ram, PC + 6, OP_NOP); /* 終了 */
    put16(ram, PC + 8, OP_NOP);
    put16(ram, IRQ_HANDLER, OP_RTE);
    mc68k_state_t *cpu = make_cpu(ram);
    mc68k_reset(cpu);
    mcp_perf_clear();
    assert(mcp_perf_arm(cpu, PC, PC + 6) == 0);
    int waiting = 0;
    for (int i = 0; i < 40; i++) {
        run_chunk(cpu, MAIN_CHUNK, &waiting);
        if (waiting == STOP_ITERS_BEFORE_IRQ)
            mc68k_set_irq(cpu, 1);
    }
    assert(waiting >= STOP_ITERS_BEFORE_IRQ);
    mcp_perf_status_t st;
    mcp_perf_get_status(&st);
    assert(st.state == MCP_PERF_COMPLETE);
    uint64_t cpu_delta = st.end.cpu - st.start.cpu;
    uint64_t elapsed_delta = st.end.elapsed - st.start.elapsed;
    assert(elapsed_delta > cpu_delta);
    assert(elapsed_delta - cpu_delta >= (uint64_t)(STOP_ITERS_BEFORE_IRQ - 1) * MAIN_CHUNK);
    mcp_perf_clear();
    mc68k_destroy(cpu);
}

/* 終了 PC 先着は INVALID。コールバック内では解除せず、dbg_exec() 復帰後に解除する。 */
static void test_end_before_start_invalid(void) {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu = make_nop_cpu(ram, 16);
    mcp_perf_clear();
    assert(mcp_perf_arm(cpu, PC + 10, PC + 4) == 0);
    mc68k_execute(cpu, 100);
    mcp_perf_status_t st;
    mcp_perf_get_status(&st);
    assert(st.state == MCP_PERF_INVALID);
    assert(mcp_perf_owns_probe()); /* 解除は復帰後 */
    mcp_perf_after_exec();
    assert(!mcp_perf_owns_probe());
    mcp_perf_get_status(&st);
    assert(st.state == MCP_PERF_INVALID && st.start_pc == PC + 10 && st.end_pc == PC + 4);
    mc68k_destroy(cpu);
}

static void test_arm_validation_and_lifecycle(void) {
    uint8_t ram[RAM_SIZE];
    mc68k_state_t *cpu = make_nop_cpu(ram, 16);
    mcp_perf_clear();
    assert(mcp_perf_arm(NULL, PC, PC + 2) == MCP_PERF_ARM_BADARG);
    assert(mcp_perf_arm(cpu, PC + 1, PC + 2) == MCP_PERF_ARM_BADARG);
    assert(mcp_perf_arm(cpu, PC, PC + 3) == MCP_PERF_ARM_BADARG);
    assert(mcp_perf_arm(cpu, PC, PC) == MCP_PERF_ARM_BADARG);
    assert(!mcp_perf_owns_probe());

    mcp_perf_status_t st;
    assert(mcp_perf_arm(cpu, PC + 2, PC + 8) == 0);
    mcp_perf_get_status(&st);
    uint64_t seq = st.sequence;
    assert(st.state == MCP_PERF_WAIT_START && mcp_perf_owns_probe());

    /* RUNNING 中の開始 PC 再到達は同一世代の再計測。 */
    mc68k_execute(cpu, 100);
    mcp_perf_get_status(&st);
    assert(st.state == MCP_PERF_COMPLETE && st.sequence == seq);
    assert(mcp_perf_owns_probe()); /* COMPLETE はプローブを保持する */

    /* COMPLETE は invalidate で変わらない。 */
    mcp_perf_invalidate();
    mcp_perf_get_status(&st);
    assert(st.state == MCP_PERF_COMPLETE);

    /* 再 ARM は前回ラッチを破棄して世代を進める。 */
    assert(mcp_perf_arm(cpu, PC + 2, PC + 8) == 0);
    mcp_perf_get_status(&st);
    assert(st.state == MCP_PERF_WAIT_START && st.sequence == seq + 1);
    assert(st.start.cpu == 0 && st.end.cpu == 0);

    /* WAIT_START/RUNNING の invalidate は INVALID にしてプローブを解除する。 */
    mcp_perf_invalidate();
    mcp_perf_get_status(&st);
    assert(st.state == MCP_PERF_INVALID && !mcp_perf_owns_probe());

    /* リセットは IDLE へ戻すが累積は保持する。 */
    mcp_perf_account(10, 20);
    mcp_perf_status_t before;
    mcp_perf_get_status(&before);
    mcp_perf_reset();
    mcp_perf_get_status(&st);
    assert(st.state == MCP_PERF_IDLE && st.total.cpu == before.total.cpu &&
           st.total.elapsed == before.total.elapsed);
    mc68k_destroy(cpu);
}

static void test_account_ignores_nonpositive_cpu(void) {
    mcp_perf_status_t a, b;
    mcp_perf_get_status(&a);
    mcp_perf_account(0, 5);
    mcp_perf_account(-1, 0);
    mcp_perf_get_status(&b);
    assert(b.total.cpu == a.total.cpu && b.total.elapsed == a.total.elapsed + 5);
}

int main(void) {
    test_boundary_and_determinism();
    test_totals_independent_of_probe();
    test_stop_elapsed_exceeds_cpu();
    test_end_before_start_invalid();
    test_arm_validation_and_lifecycle();
    test_account_ignores_nonpositive_cpu();
    return 0;
}
