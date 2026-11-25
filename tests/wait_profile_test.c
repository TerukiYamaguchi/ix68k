/*
 * システムポート#5 ($E8E009, X68030) 由来のRAM/ROMウェイトプロファイルと
 * mc68k_bus_t.access_timing フックの単体テスト
 * （docs/ix68k-internals/design/cpu_design.md §2）。
 */
#include "bus/bus_internal.h"
#include "mc68000/cpu.h"
#include <assert.h>
#include <stdio.h>

static mc68k_bus_timing_t query_fc(mc68k_bus_t *hooks, uint32_t addr, uint8_t fc,
                                   unsigned size, bool instruction, bool read) {
    const mc68k_bus_access_t a = {
        .physical = addr, .size = size, .fc = fc, .read = read, .instruction = instruction};
    mc68k_bus_timing_t out = {0xFFFFu, false}; /* 呼ばれなければ検出できる番兵値 */
    hooks->access_timing(hooks->ctx, &a, &out);
    return out;
}

static mc68k_bus_timing_t query(mc68k_bus_t *hooks, uint32_t addr, bool instruction,
                                bool read) {
    return query_fc(hooks, addr, 5, 2, instruction, read); /* FC=5: スーパバイザデータ */
}

static void check_profile_rows(x68k_bus_t *bus, mc68k_bus_t *hooks) {
    static const x68k_timing_profile_t profiles[] = {
        X68K_TIMING_PROFILE_X68000, X68K_TIMING_PROFILE_XVI, X68K_TIMING_PROFILE_X68030,
        X68K_TIMING_PROFILE_060TURBO};
    static const struct {
        uint32_t addr;
        uint16_t wait[4][2];
    } rows[] = {
        {0x000100u, {{0, 0}, {0, 0}, {17, 17}, {17, 17}}},
        {0x00F00100u, {{1, 1}, {4, 4}, {17, 17}, {17, 17}}},
        {0x00ED0000u, {{1, 1}, {4, 4}, {5, 5}, {13, 13}}},
        {0x00C00000u, {{1, 1}, {4, 4}, {9, 9}, {21, 21}}},
        {0x00E00000u, {{2, 2}, {6, 6}, {9, 9}, {22, 22}}},
        {0x00E80000u, {{1, 1}, {4, 4}, {9, 9}, {21, 21}}},
        {0x00E82000u, {{3, 3}, {7, 7}, {14, 14}, {33, 33}}},
        {0x00E82400u, {{1, 1}, {3, 3}, {9, 11}, {21, 21}}},
        {0x00E84000u, {{15, 15}, {26, 26}, {37, 31}, {73, 73}}},
        {0x00E88000u, {{4, 4}, {9, 9}, {24, 24}, {47, 47}}},
        {0x00E8A000u, {{2, 2}, {5, 5}, {31, 31}, {77, 77}}},
        {0x00E8C000u, {{1, 1}, {4, 4}, {7, 7}, {17, 17}}},
        {0x00E8E000u, {{1, 1}, {4, 4}, {7, 7}, {17, 17}}},
        {0x00E90000u, {{2, 2}, {5, 5}, {19, 53}, {37, 37}}},
        {0x00E92000u, {{2, 2}, {5, 5}, {19, 19}, {37, 37}}},
        {0x00E94000u, {{2, 2}, {5, 5}, {19, 19}, {37, 37}}},
        {0x00E96000u, {{2, 2}, {5, 5}, {19, 19}, {37, 37}}},
        {0x00E98000u, {{6, 6}, {12, 12}, {41, 49}, {97, 97}}},
        {0x00E9A000u, {{2, 2}, {5, 5}, {19, 19}, {37, 37}}},
        {0x00E9C000u, {{2, 2}, {5, 5}, {19, 19}, {37, 37}}},
        {0x00EB0000u, {{1, 1}, {4, 4}, {9, 9}, {21, 21}}},
        {0x00EB8000u, {{4, 5}, {9, 10}, {17, 18}, {37, 39}}},
    };

    for (size_t profile = 0; profile < sizeof(profiles) / sizeof(profiles[0]); profile++) {
        assert(x68k_bus_set_timing_profile(bus, profiles[profile]));
        for (size_t row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
            bool cacheable = profile >= 2 && row < 2;
            mc68k_bus_timing_t t = query(hooks, rows[row].addr, false, true);
            assert(t.cacheable == cacheable && t.wait_cycles == rows[row].wait[profile][0]);
            t = query(hooks, rows[row].addr, false, false);
            assert(t.cacheable == cacheable && t.wait_cycles == rows[row].wait[profile][1]);
        }
    }
}

/* 領域境界の両側、long の倍率、FC=7、none を全 profile で確認する。 */
static void check_boundaries(x68k_bus_t *bus, mc68k_bus_t *hooks) {
    assert(x68k_bus_set_timing_profile(bus, X68K_TIMING_PROFILE_X68000));
    static const struct {
        uint32_t addr;
        uint16_t read, write;
    } edges[] = {
        {0x00E81FFFu, 1, 1},
        {0x00E82000u, 3, 3}, /* CRTC | VC palette */
        {0x00E823FFu, 3, 3},
        {0x00E82400u, 1, 1}, /* VC palette | VC mode */
        {0x00EB7FFFu, 1, 1},
        {0x00EB8000u, 4, 5}, /* sprite reg | sprite VRAM */
        {0x00ECFFFFu, 0, 0}, /* 未割当 */
        {SRAM_BASE, 1, 1},
        {SRAM_BASE + SRAM_SIZE - 1u, 1, 1},
        {SRAM_BASE + SRAM_SIZE, 0, 0}, /* SRAM 末尾の外 */
        {0x00E8AFFFu, 2, 2},           /* RTC 領域内の奇数 byte 側も課金 */
        {0x00E8A000u, 2, 2},           /* RTC 偶数 byte（バスエラー）も課金 */
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        mc68k_bus_timing_t t = query(hooks, edges[i].addr, false, true);
        assert(!t.cacheable && t.wait_cycles == edges[i].read);
        t = query(hooks, edges[i].addr, false, false);
        assert(!t.cacheable && t.wait_cycles == edges[i].write);
    }

    /* long の倍率: x68000/xvi は 2 倍、x68030/060turbo は 1 倍。 */
    static const struct {
        x68k_timing_profile_t profile;
        uint16_t word, lng;
    } longs[] = {
        {X68K_TIMING_PROFILE_X68000, 1, 2},
        {X68K_TIMING_PROFILE_XVI, 4, 8},
        {X68K_TIMING_PROFILE_X68030, 9, 9},
        {X68K_TIMING_PROFILE_060TURBO, 21, 21},
    };
    for (size_t i = 0; i < sizeof(longs) / sizeof(longs[0]); i++) {
        assert(x68k_bus_set_timing_profile(bus, longs[i].profile));
        mc68k_bus_timing_t t = query_fc(hooks, 0x00C00000u, 5, 2, false, true);
        assert(t.wait_cycles == longs[i].word);
        t = query_fc(hooks, 0x00C00000u, 5, 4, false, true);
        assert(t.wait_cycles == longs[i].lng);
        /* FC=7 は profile を問わず 0 / non-cacheable。 */
        t = query_fc(hooks, 0x00C00000u, MC68K_FC_CPU_SPACE, 2, false, true);
        assert(!t.cacheable && t.wait_cycles == 0);
        t = query_fc(hooks, 0x00C00000u, MC68K_FC_CPU_SPACE, 4, false, false);
        assert(!t.cacheable && t.wait_cycles == 0);
    }

    /* none は全幅で 0。範囲外 profile は設定を拒否して直前の profile を保つ。 */
    assert(x68k_bus_set_timing_profile(bus, X68K_TIMING_PROFILE_NONE));
    for (unsigned size = 1; size <= 4; size *= 2) {
        mc68k_bus_timing_t t = query_fc(hooks, 0x00E84000u, 5, size, false, true);
        assert(!t.cacheable && t.wait_cycles == 0);
    }
    assert(!x68k_bus_set_timing_profile(bus, X68K_TIMING_PROFILE_COUNT));
    assert(query(hooks, 0x00E84000u, false, true).wait_cycles == 0);
}

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_set_local_memory(b, 16));
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);

    /* 1. access_timing が配線されていること（後方互換のNULLではない）。 */
    assert(hooks.access_timing != NULL);
    assert(hooks.read_byte != NULL && hooks.write_byte != NULL);

    /* 2. 既定値（x68k_bus_create() 直後）はポート$00相当:
     *    rom=wait_penalty(0+2)=2, ram=wait_penalty(X68K_RAM_WAIT_STATIC_COLUMN)=0 */
    assert(b->wait.rom == 2 && b->wait.ram == 0 && b->wait.mmio == 0);

    /* 3. $E8E009 への書込みで rom/ram が cpu_design.md の式どおりに再計算される。 */
    struct {
        uint8_t val, rom, ram;
    } cases[] = {
        {0x00u, 2, 0},   /* rom_n=0->+2, ram_n=0->スタティックカラム近似=0 */
        {0x04u, 2, 6},   /* ram_n=4->+2=6 (16MHz相当) */
        {0x0Au, 2, 12},  /* ram_n=10->+2=12 (10MHz相当) */
        {0xF0u, 17, 0},  /* rom_n=15->+2=17, ram_n=0->0 */
        {0xFFu, 17, 17}, /* rom_n=15->17, ram_n=15->+2=17 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool ok = hooks.write_byte(hooks.ctx, 0xE8E009u, cases[i].val);
        assert(ok);
        assert(b->wait.rom == cases[i].rom && b->wait.ram == cases[i].ram && b->wait.mmio == 0);
        /* 4. 読み戻しが書いた値を返すこと（既存のCRTC汎用バッキング流用）。 */
        uint8_t back = 0;
        assert(hooks.read_byte(hooks.ctx, 0xE8E009u, &back) && back == cases[i].val);
    }

    /* 5a. b->cpu が未配線の間、X68030専有ポートの効果は一切出ない
     * （wait.romは0xFFケースにより非ゼロ=17のはずだが、cpu未配線なら
     * cacheable/waitとも既定の0を返す）。IOCS ROM領域で確認する。 */
    assert(b->wait.rom == 17);
    mc68k_bus_timing_t t = query(&hooks, 0x00F00100u, true, true);
    assert(!t.cacheable && t.wait_cycles == 0);

    /* 5b. 無印X68000 profile は CPU 接続後に固定表を適用する。 */
    mc68k_state_t *cpu68000 = mc68k_create_model(&hooks, MC68K_MODEL_68000);
    assert(cpu68000 != NULL);
    x68k_bus_set_cpu(b, cpu68000);
    t = query(&hooks, 0x00F00100u, true, true);
    assert(!t.cacheable && t.wait_cycles == 1);

    mc68k_destroy(cpu68000);

    /* 5c. X68030モデルを配線すると、bus_access_timing() の領域分類が働く。
     * RAM/IOCS ROM(SCSI ROM窓含む)は cacheable=true、それ以外は
     * cacheable=false かつ wait=0（cpu_design.md §2）。ポートはケース5(0xFF)のままなので
     * rom=17。 */
    mc68k_state_t *cpu030 = mc68k_create_model(&hooks, MC68K_MODEL_68030);
    assert(cpu030 != NULL);
    x68k_bus_set_cpu(b, cpu030);
    assert(x68k_bus_set_timing_profile(b, X68K_TIMING_PROFILE_X68030));

    /* 全領域の read/write 値、CPU空間、long、local memory を確認する。 */
    check_profile_rows(b, &hooks);
    check_boundaries(b, &hooks);
    assert(x68k_bus_set_timing_profile(b, X68K_TIMING_PROFILE_X68030));
    t = query_fc(&hooks, 0x22000u, MC68K_FC_CPU_SPACE, 2, false, true);
    assert(!t.cacheable && t.wait_cycles == 0);
    t = query_fc(&hooks, 0x22000u, 5, 2, false, true);
    assert(t.cacheable && t.wait_cycles == b->wait.ram);
    t = query_fc(&hooks, 0x00E84000u, 5, 4, false, true);
    assert(!t.cacheable && t.wait_cycles == 37);
    assert(x68k_bus_set_timing_profile(b, X68K_TIMING_PROFILE_XVI));
    t = query_fc(&hooks, 0x00E84000u, 5, 4, false, true);
    assert(!t.cacheable && t.wait_cycles == 52);
    t = query(&hooks, 0x10000000u, false, true);
    assert(!t.cacheable && t.wait_cycles == 0);
    t = query(&hooks, 0x01E90000u, false, true);
    assert(!t.cacheable && t.wait_cycles == 5);
    assert(x68k_bus_set_timing_profile(b, X68K_TIMING_PROFILE_NONE));
    t = query(&hooks, 0x00E84000u, false, true);
    assert(!t.cacheable && t.wait_cycles == 0);
    assert(x68k_bus_set_timing_profile(b, X68K_TIMING_PROFILE_X68030));

    /* 5d. 68040 はX68030専用のポート#5 wait近似を継承しない。040自身のcache/MMU
     * モデルに任せるため、RAM/ROMを含めてこのフックは常に無課金で返す。 */
    mc68k_destroy(cpu030);
    mc68k_state_t *cpu040 = mc68k_create_model(&hooks, MC68K_MODEL_68040);
    assert(cpu040 != NULL);
    x68k_bus_set_cpu(b, cpu040);
    assert(x68k_bus_set_timing_profile(b, X68K_TIMING_PROFILE_NONE));
    t = query(&hooks, 0x000100u, true, true);
    assert(!t.cacheable && t.wait_cycles == 0);
    t = query(&hooks, 0x00F00100u, true, true);
    assert(!t.cacheable && t.wait_cycles == 0);

    /* 6. x68k_bus_reset_peripheral()（CPU RESET命令相当）後、ポートは$00相当
     * へ戻る（cpu_design.md §2）。 */
    x68k_bus_reset_peripheral(b);
    assert(b->wait.rom == 2 && b->wait.ram == 0 && b->wait.mmio == 0);
    uint8_t back = 0xAAu;
    assert(hooks.read_byte(hooks.ctx, 0xE8E009u, &back) && back == 0x00u);

    mc68k_destroy(cpu040);
    x68k_bus_destroy(b);
    printf("wait_profile_test: OK\n");
    return 0;
}
