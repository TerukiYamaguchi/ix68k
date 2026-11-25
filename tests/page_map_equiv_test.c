/*
 * 物理ページ表（x68k_page_map_t）あり／なしの状態一致試験。
 * 実ホストのバス（x68k_bus_t）と 68060 を同じ構成で 2 つ作り、一方だけ page_map を外して
 * 同じプログラムを走らせる。各命令後のレジスタと消費サイクル、実行後の RAM・ローカルメモリ・
 * GVRAM・SRAM の内容が一致することを確かめる。
 *
 * プログラムは RAM・ローカルメモリ・GVRAM・ROM（書き込みは無視される）・MMIO・隙間を、
 * byte/word/long で読み書きする。ユーザモード保護と ram_watch の有無、実行中の保護切り替えも
 * 組み合わせる（これらは表の作り直し契機）。
 */
#include "bus/bus_internal.h"
#include "mc68000/cpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_MB        16u
#define LOCAL_BASE      0x10000000u
#define PROG_BASE       0x4000u
#define SSP_INIT        0x7F00u
#define VEC_RESET_SP    0x0u
#define VEC_RESET_PC    0x4u
#define VEC_BUS_ERROR   8u
#define VEC_ADDR_ERROR  12u
#define HANDLER_PC      0x3F00u
#define MAX_STEPS       4000u
#define SR_USER_MODE    0x0000u
#define OP_STOP         0x4E72u
#define STOP_SR         0x2700u
#define USER_AREA_LIMIT 0x2000u

typedef struct {
    uint16_t w[512];
    unsigned n;
} prog_t;

static void emit(prog_t *p, uint16_t v) {
    assert(p->n < sizeof p->w / sizeof p->w[0]);
    p->w[p->n++] = v;
}
static void emit_long(prog_t *p, uint32_t v) {
    emit(p, (uint16_t)(v >> 16));
    emit(p, (uint16_t)v);
}
/* MOVE.<size> Dn,(abs).L / MOVE.<size> (abs).L,Dn */
static void st_b(prog_t *p, unsigned dn, uint32_t a) {
    emit(p, (uint16_t)(0x13C0u | dn));
    emit_long(p, a);
}
static void st_w(prog_t *p, unsigned dn, uint32_t a) {
    emit(p, (uint16_t)(0x33C0u | dn));
    emit_long(p, a);
}
static void st_l(prog_t *p, unsigned dn, uint32_t a) {
    emit(p, (uint16_t)(0x23C0u | dn));
    emit_long(p, a);
}
static void ld_b(prog_t *p, unsigned dn, uint32_t a) {
    emit(p, (uint16_t)(0x1039u | (dn << 9)));
    emit_long(p, a);
}
static void ld_w(prog_t *p, unsigned dn, uint32_t a) {
    emit(p, (uint16_t)(0x3039u | (dn << 9)));
    emit_long(p, a);
}
static void ld_l(prog_t *p, unsigned dn, uint32_t a) {
    emit(p, (uint16_t)(0x2039u | (dn << 9)));
    emit_long(p, a);
}
static void moveq(prog_t *p, unsigned dn, int8_t v) {
    emit(p, (uint16_t)(0x7000u | (dn << 9) | (uint8_t)v));
}
static void tas(prog_t *p, uint32_t a) {
    emit(p, 0x4AF9u);
    emit_long(p, a);
}
static void to_user(prog_t *p) {
    emit(p, 0x46FCu); /* MOVE #imm,SR */
    emit(p, SR_USER_MODE);
}
static void stop(prog_t *p) {
    emit(p, OP_STOP);
    emit(p, STOP_SR);
}

/* 巡回するアドレス群。 */
static const uint32_t kAddrs[] = {
    0x00FF00u,         /* RAM（ユーザモード保護の範囲外） */
    0x0FFFF0u,         /* RAM 終端付近（既定 1MB） */
    0x001000u,         /* RAM 低位（保護範囲内。保護中のユーザモードはここでバスエラー） */
    0x001FFEu,         /* ページまたぎ */
    0xC00000u,         /* GVRAM */
    0xC01FFEu,         /* GVRAM ページまたぎ */
    0xE00000u,         /* TVRAM */
    0xE88001u,         /* MFP */
    0xE84080u + 0x0Cu, /* DMAC ch2 MAR */
    0xED0100u,         /* SRAM */
    0xF00100u,         /* IOCS ROM */
    0xFC1000u,         /* SCSI ROM 窓 */
    LOCAL_BASE + 0x100u,
    LOCAL_BASE + 0xFFEu, /* ローカルメモリのページまたぎ */
    LOCAL_BASE + 0x800000u,
};
#define NADDR (sizeof kAddrs / sizeof kAddrs[0])

/* 4KB 境界をまたぐアクセス（偶数アドレスのみ。68000 でもアドレスエラーにならない）。
 * 0xDFFFFE の long は GVRAM 終端から TVRAM へまたぐ。 */
static const uint32_t kCrossAddrs[] = {
    0x001FFEu,           /* RAM 低位 */
    0x00FFFEu,           /* RAM */
    0xC00FFEu,           /* GVRAM ページ境界 */
    0xC01FFEu,           /* GVRAM ページ境界 */
    0xDFFFFEu,           /* GVRAM 終端 → TVRAM */
    LOCAL_BASE + 0xFFEu, /* ローカルメモリ */
};
#define NCROSS (sizeof kCrossAddrs / sizeof kCrossAddrs[0])

static void body_cross(prog_t *p) {
    moveq(p, 0, 0x11);
    moveq(p, 1, 0x7A);
    moveq(p, 2, (int8_t)0xA5);
    for (unsigned i = 0; i < NCROSS; i++) {
        const uint32_t a = kCrossAddrs[i];
        st_w(p, 1, a);
        st_l(p, 2, a);
        ld_w(p, 4, a);
        ld_l(p, 5, a);
        emit(p, 0xD085u); /* ADD.L D5,D0 */
    }
}

/* 全アドレスへ byte/word/long の書き込みと読み戻しを行い、TAS も混ぜる。 */
static void body(prog_t *p) {
    moveq(p, 0, 0x11);
    moveq(p, 1, 0x7A);
    moveq(p, 2, (int8_t)0xA5);
    moveq(p, 3, 0x3C);
    for (unsigned i = 0; i < NADDR; i++) {
        const uint32_t a = kAddrs[i];
        st_b(p, 0, a);
        st_w(p, 1, a + 2u);
        st_l(p, 2, a + 4u);
        ld_b(p, 4, a);
        ld_w(p, 5, a + 2u);
        ld_l(p, 6, a + 4u);
        emit(p, 0xD086u); /* ADD.L D6,D0（読み戻しをレジスタへ反映） */
    }
    tas(p, 0x2000u);
    tas(p, LOCAL_BASE + 0x2000u);
    st_w(p, 1, 0x100000u); /* RAM 外（隙間）。バスエラーでハンドラの STOP に入る */
}

typedef enum {
    SC_PLAIN,
    SC_USER,
    SC_USER_PROTECT,
    SC_WATCH,
    SC_TOGGLE,
    SC_CROSS,
    SC_CROSS_TVRAM_MASK,
    SC_GV_USER_B, /* 保護中のユーザモードで GVRAM を最初に触る（バスエラーを確かめる） */
    SC_GV_USER_W,
    SC_GV_USER_L,
    SC_WATCH_GVRAM, /* ram_watch の範囲が GVRAM に重なる */
    SC_COUNT
} scenario_t;
static const char *const kScenario[] = {"plain",     "user",      "user+protect", "watch",
                                        "toggle",    "cross",     "cross+tvmask", "gv-user-b",
                                        "gv-user-w", "gv-user-l", "watch-gvram"};

typedef struct {
    const char *name;
    mc68k_model_t model;
    x68k_timing_profile_t profile;
} config_t;
static const config_t kConfigs[] = {
    {"68060/060turbo", MC68K_MODEL_68060, X68K_TIMING_PROFILE_060TURBO},
    {"68000/x68000", MC68K_MODEL_68000, X68K_TIMING_PROFILE_X68000},
    {"68030/x68030", MC68K_MODEL_68030, X68K_TIMING_PROFILE_X68030},
};

typedef struct {
    mc68k_regs_t regs[MAX_STEPS];
    int cycles[MAX_STEPS];
    unsigned steps;
    uint8_t *ram;
    uint8_t *local;
    uint8_t *gvram;
    uint8_t *tvram;
    uint8_t *sram;
    uint8_t *rom;
} result_t;

static void put_word(uint8_t *ram, uint32_t a, uint16_t v) {
    ram[a] = (uint8_t)(v >> 8);
    ram[a + 1u] = (uint8_t)v;
}
static void put_long(uint8_t *ram, uint32_t a, uint32_t v) {
    put_word(ram, a, (uint16_t)(v >> 16));
    put_word(ram, a + 2u, (uint16_t)v);
}

static void fill_state(x68k_bus_t *b) {
    uint32_t x = 4242u;
    for (uint32_t i = 0; i < RAM_BUF_SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        b->ram[i] = (uint8_t)(x >> 24);
    }
    for (uint32_t i = 0; i < b->local_memory_size; i++) {
        x = x * 1664525u + 1013904223u;
        b->local_memory[i] = (uint8_t)(x >> 24);
    }
    for (uint32_t i = 0; i < GVRAM_SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        b->gvram[i] = (uint8_t)(x >> 24);
    }
    for (uint32_t i = 0; i < TVRAM_SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        b->tvram[i] = (uint8_t)(x >> 24);
    }
    for (uint32_t i = 0; i < IOCS_ROM_SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        b->iocs_rom[i] = (uint8_t)(x >> 24);
    }
}

static void run(const config_t *cfg, scenario_t sc, bool use_table, result_t *out) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b);
    assert(x68k_bus_set_local_memory(b, LOCAL_MB));
    fill_state(b);
    put_long(b->ram, VEC_RESET_SP, SSP_INIT);
    put_long(b->ram, VEC_RESET_PC, PROG_BASE);
    put_long(b->ram, VEC_BUS_ERROR, HANDLER_PC);
    put_long(b->ram, VEC_ADDR_ERROR, HANDLER_PC);
    put_word(b->ram, HANDLER_PC, OP_STOP);
    put_word(b->ram, HANDLER_PC + 2u, STOP_SR);
    prog_t prog;
    memset(&prog, 0, sizeof prog);
    if (sc == SC_USER || sc == SC_USER_PROTECT || sc == SC_GV_USER_B || sc == SC_GV_USER_W ||
        sc == SC_GV_USER_L)
        to_user(&prog);
    if (sc == SC_CROSS || sc == SC_CROSS_TVRAM_MASK) {
        body_cross(&prog);
    } else if (sc == SC_GV_USER_B || sc == SC_GV_USER_W || sc == SC_GV_USER_L) {
        moveq(&prog, 1, 0x7A);
        /* 最初の GVRAM アクセスでバスエラーになり、ハンドラの STOP で終わる */
        if (sc == SC_GV_USER_B)
            st_b(&prog, 1, 0xC00000u);
        else if (sc == SC_GV_USER_W)
            st_w(&prog, 1, 0xC00000u);
        else
            st_l(&prog, 1, 0xC00000u);
        ld_l(&prog, 4, 0xC00000u);
    } else {
        body(&prog);
    }
    if (sc == SC_TOGGLE) {
        /* 保護と watch の切り替えはホストが命令の合間に行う（下のループ）。 */
    }
    stop(&prog);
    for (unsigned i = 0; i < prog.n; i++)
        put_word(b->ram, PROG_BASE + 2u * i, prog.w[i]);

    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    if (!use_table)
        hooks.page_map = NULL;
    mc68k_state_t *cpu = mc68k_create_model(&hooks, cfg->model);
    assert(cpu);
    x68k_bus_set_cpu(b, cpu);
    assert(x68k_bus_set_timing_profile(b, cfg->profile));
    mc68k_reset(cpu);
    if (sc == SC_USER_PROTECT || sc == SC_TOGGLE || sc == SC_GV_USER_B || sc == SC_GV_USER_W ||
        sc == SC_GV_USER_L)
        x68k_bus_set_super_protect(b, 1);
    if (sc == SC_CROSS_TVRAM_MASK) {
        /* TVRAM へまたぐ long 書き込みがバイトごとに R21 の SA/AP と R23 のマスクを通る */
        b->crtc_regs[0x2A] = 0x03u; /* SA ON + マスク適用 */
        b->crtc_regs[0x2B] = 0x30u; /* AP = プレーン 0,1 */
        b->crtc_regs[0x2E] = 0xF0u;
        b->crtc_regs[0x2F] = 0x0Fu;
    }
    if (sc == SC_USER_PROTECT) {
        /* 低位 RAM の保護範囲を $2000 未満に絞る。ユーザモードは $00FF00 付近の RAM
         * を直接読み書きでき、 $001000（保護対象）に触れた時点でバスエラーになる。 */
        b->super_area_limit = USER_AREA_LIMIT;
        x68k_bus_page_map_refresh(b);
    }
    if (sc == SC_WATCH)
        x68k_bus_set_ram_watch(b, 0x1000u, 0x2000u);
    if (sc == SC_WATCH_GVRAM)
        x68k_bus_set_ram_watch(b, 0xBFF000u, 0x4000u);

    memset(out->regs, 0, sizeof out->regs);
    out->steps = 0;
    while (out->steps < MAX_STEPS) {
        if (sc == SC_TOGGLE) {
            /* 5 命令ごとに保護と watch を切り替える（作り直しの契機を実行中に踏む）。 */
            if (out->steps % 5u == 0u) {
                x68k_bus_set_super_protect(b, (int)((out->steps / 5u) & 1u));
                x68k_bus_set_ram_watch(b, ((out->steps / 5u) & 2u) ? 0x1000u : 0u,
                                       ((out->steps / 5u) & 2u) ? 0x2000u : 0u);
            }
        }
        out->cycles[out->steps] = mc68k_execute(cpu, 1);
        mc68k_get_regs(cpu, &out->regs[out->steps]);
        out->steps++;
        if (mc68k_get_exec_state(cpu) != CPU_NORMAL)
            break;
    }
    assert(out->steps < MAX_STEPS);
    out->ram = malloc(RAM_BUF_SIZE);
    out->local = malloc(b->local_memory_size);
    out->gvram = malloc(GVRAM_SIZE);
    out->tvram = malloc(TVRAM_SIZE);
    out->sram = malloc(SRAM_SIZE);
    out->rom = malloc(IOCS_ROM_SIZE);
    assert(out->ram && out->local && out->gvram && out->tvram && out->sram && out->rom);
    memcpy(out->ram, b->ram, RAM_BUF_SIZE);
    memcpy(out->local, b->local_memory, b->local_memory_size);
    memcpy(out->gvram, b->gvram, GVRAM_SIZE);
    memcpy(out->tvram, b->tvram, TVRAM_SIZE);
    memcpy(out->sram, b->sram, SRAM_SIZE);
    memcpy(out->rom, b->iocs_rom, IOCS_ROM_SIZE);
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

static void release(result_t *r) {
    free(r->ram);
    free(r->local);
    free(r->gvram);
    free(r->tvram);
    free(r->sram);
    free(r->rom);
}

static result_t base_r, tab_r;

int main(void) {
    assert(sizeof kScenario / sizeof kScenario[0] == SC_COUNT);
    for (unsigned ci = 0; ci < sizeof kConfigs / sizeof kConfigs[0]; ci++)
        for (unsigned sc = 0; sc < SC_COUNT; sc++) {
            const config_t *cfg = &kConfigs[ci];
            run(cfg, (scenario_t)sc, false, &base_r);
            run(cfg, (scenario_t)sc, true, &tab_r);
            bool ok = base_r.steps == tab_r.steps;
            for (unsigned i = 0; ok && i < base_r.steps; i++) {
                ok = base_r.cycles[i] == tab_r.cycles[i] &&
                     memcmp(&base_r.regs[i], &tab_r.regs[i], sizeof base_r.regs[i]) == 0;
                if (!ok)
                    fprintf(stderr, "step %u differs (%s, scenario %s)\n", i, cfg->name,
                            kScenario[sc]);
            }
            ok = ok && memcmp(base_r.ram, tab_r.ram, RAM_BUF_SIZE) == 0;
            ok = ok && memcmp(base_r.local, tab_r.local, (size_t)LOCAL_MB << 20) == 0;
            ok = ok && memcmp(base_r.gvram, tab_r.gvram, GVRAM_SIZE) == 0;
            ok = ok && memcmp(base_r.tvram, tab_r.tvram, TVRAM_SIZE) == 0;
            ok = ok && memcmp(base_r.sram, tab_r.sram, SRAM_SIZE) == 0;
            printf("%s %s: %u steps pc=%08X\n", cfg->name, kScenario[sc], base_r.steps,
                   base_r.regs[base_r.steps - 1].pc);
            ok = ok && memcmp(base_r.rom, tab_r.rom, IOCS_ROM_SIZE) == 0;
            if (!ok)
                fprintf(stderr, "MISMATCH: %s scenario %s\n", cfg->name, kScenario[sc]);
            assert(ok);
            release(&base_r);
            release(&tab_r);
        }
    printf("page_map_equiv_test: OK\n");
    return 0;
}
