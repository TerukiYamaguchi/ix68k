/*
 * 物理ページ表（x68k_page_map_t、mc68000 の mc68k_page_map_t）の突き合わせ試験。
 * 表の各エントリを、ホストの既存コールバック（bus_read_*、bus_access_timing）と全 4KB ページで
 * 比べる。また、表の入力が変わる契機（SRAM の ram_limit、エリアセット、拡張エリア、ウェイト
 * ポート、ユーザモード保護、timing profile、DMAC ch2、リセット、スナップショット復元）の後に、
 * 表が状態から作り直した結果と一致している（作り直し漏れがない）ことを確かめる。
 * 書き込みの直接経路（MC68K_PAGE_WRITE_DIRECT）は、コールバックで書いた結果が host に現れること
 * （RAM のみ）、ROM は書いても変わらないこと、ram_watch
 * の範囲と重なるページから外れることも見る。
 */
#include "bus/bus_internal.h"
#include "bus/snapshot.h"
#include "mc68000/cpu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRAM_RAM_LIMIT_ADDR 0xED0008u
#define AREA_SET_ADDR       0xE86001u
#define EXT_AREA_ADDR       0xEAFF81u
#define WAIT_PORT_ADDR      0xE8E009u
#define LOCAL_MB            16u
#define CHECK_OFFSETS       4u

static const uint32_t kOffsets[CHECK_OFFSETS] = {0u, 2u, 4u, MC68K_PAGE_MASK - 3u};

static void set_ram_limit(x68k_bus_t *b, uint32_t limit) {
    for (unsigned i = 0; i < 4; i++)
        x68k_bus_write8(b, SRAM_RAM_LIMIT_ADDR + i, (uint8_t)(limit >> (24u - 8u * i)));
}

static void fill_memory(x68k_bus_t *b) {
    uint32_t x = 12345u;
    for (uint32_t i = 0; i < RAM_BUF_SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        b->ram[i] = (uint8_t)(x >> 24);
    }
    for (uint32_t i = 0; i < IOCS_ROM_SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        b->iocs_rom[i] = (uint8_t)(x >> 24);
    }
    for (uint32_t i = 0; i < b->local_memory_size; i++) {
        x = x * 1664525u + 1013904223u;
        b->local_memory[i] = (uint8_t)(x >> 24);
    }
}

/* 1 エントリを、コールバックと突き合わせる。 */
static unsigned check_entry(x68k_bus_t *b, const mc68k_bus_t *hooks,
                            const mc68k_page_map_t *map, const mc68k_page_entry_t *e,
                            uint32_t phys, unsigned *timing_checked) {
    unsigned direct_checked = 0;
    for (unsigned k = 0; k < CHECK_OFFSETS; k++) {
        const uint32_t addr = phys + kOffsets[k];
        if (e->flags & MC68K_PAGE_READ_DIRECT) {
            /* スーパーバイザ専用ページも、現在の
             * CPU（リセット直後はスーパーバイザ）で突き合わせる。 */
            uint16_t w;
            uint32_t l;
            assert(hooks->read_word(hooks->ctx, addr, &w));
            assert(hooks->read_long(hooks->ctx, addr, &l));
            const uint8_t *h = e->host + kOffsets[k];
            assert(w == (uint16_t)((h[0] << 8) | h[1]));
            assert(l == (((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                         ((uint32_t)h[2] << 8) | h[3]));
            direct_checked++;
        }
        if (e->flags & MC68K_PAGE_TIMING) {
            const mc68k_page_wait_t *w = &map->wait_table[e->wait_class];
            for (unsigned rd = 0; rd < 2; rd++) {
                for (unsigned lg = 0; lg < 2; lg++) {
                    mc68k_bus_access_t a = {
                        .physical = addr, .size = lg ? 4u : 2u, .fc = 5u, .read = rd != 0};
                    mc68k_bus_timing_t t = {0, false};
                    hooks->access_timing(hooks->ctx, &a, &t);
                    const uint16_t expect =
                        lg ? (rd ? w->read_long : w->write_long) : (rd ? w->read : w->write);
                    assert(t.wait_cycles == expect);
                    assert(t.cacheable == ((e->flags & MC68K_PAGE_CACHEABLE) != 0));
                }
            }
            (*timing_checked)++;
        }
    }
    (void)b;
    return direct_checked;
}

/* WRITE_DIRECT のエントリは、コールバックで書いた結果が host に現れる（等価）。WRITE_DIRECT の
 * ないエントリ（ROM など）は、コールバックで書いても host が変わらない。host は元に戻す。 */
static void check_write_entry(const mc68k_bus_t *hooks, const mc68k_page_entry_t *e,
                              uint32_t phys, unsigned *write_checked) {
    if (!(e->flags & MC68K_PAGE_READ_DIRECT) && !(e->flags & MC68K_PAGE_WRITE_DIRECT))
        return;
    uint8_t *host = (uint8_t *)(uintptr_t)e->host;
    for (unsigned k = 0; k < CHECK_OFFSETS; k++) {
        const uint32_t addr = phys + kOffsets[k];
        uint8_t saved[4];
        memcpy(saved, host + kOffsets[k], sizeof saved);
        (void)hooks->write_long(hooks->ctx, addr, 0xA55A3CC3u);
        if (e->flags & MC68K_PAGE_WRITE_DIRECT) {
            static const uint8_t expect[4] = {0xA5u, 0x5Au, 0x3Cu, 0xC3u};
            assert(memcmp(host + kOffsets[k], expect, sizeof expect) == 0);
            memcpy(host + kOffsets[k], saved, sizeof saved);
            (void)hooks->write_word(hooks->ctx, addr, 0x1E2Du);
            assert(host[kOffsets[k]] == 0x1Eu && host[kOffsets[k] + 1u] == 0x2Du);
            (void)hooks->write_byte(hooks->ctx, addr + 1u, 0x7Fu);
            assert(host[kOffsets[k] + 1u] == 0x7Fu);
            (*write_checked)++;
        } else if ((phys & 0xFFFFFFu) >= IOCS_ROM_BASE && phys < 0x10000000u) {
            /* ROM はコールバックで書いても変わらない（RAM は ram_watch 中でも書かれる） */
            assert(memcmp(host + kOffsets[k], saved, sizeof saved) == 0);
        }
        memcpy(host + kOffsets[k], saved, sizeof saved);
    }
}

static unsigned count_write_direct(const x68k_page_map_t *pm) {
    unsigned n = 0;
    for (unsigned i = 0; i < MC68K_PAGE_L2_COUNT; i++)
        n += (pm->main[i].flags & MC68K_PAGE_WRITE_DIRECT) != 0;
    for (unsigned i = 0; i < pm->local_slots * MC68K_PAGE_L2_COUNT; i++)
        n += (pm->local[i].flags & MC68K_PAGE_WRITE_DIRECT) != 0;
    return n;
}

/* 表の全エントリを突き合わせる。載っているエントリ数を返す。 */
static void check_all(x68k_bus_t *b, const mc68k_bus_t *hooks, unsigned *direct,
                      unsigned *timing) {
    const x68k_page_map_t *pm = b->pmap;
    assert(pm);
    assert(x68k_bus_page_map_verify(b));
    *direct = 0;
    *timing = 0;
    unsigned writes = 0;
    for (unsigned i = 0; i < MC68K_PAGE_L2_COUNT; i++) {
        *direct += check_entry(b, hooks, &pm->map, &pm->main[i], i << MC68K_PAGE_SHIFT, timing);
        check_write_entry(hooks, &pm->main[i], i << MC68K_PAGE_SHIFT, &writes);
    }
    for (unsigned sl = 0; sl < pm->local_slots; sl++) {
        for (unsigned i = 0; i < MC68K_PAGE_L2_COUNT; i++) {
            const uint32_t phys =
                0x10000000u + sl * X68K_PAGE_MAP_LOCAL_SLOT_BYTES + (i << MC68K_PAGE_SHIFT);
            const mc68k_page_entry_t *e = &pm->local[(size_t)sl * MC68K_PAGE_L2_COUNT + i];
            *direct += check_entry(b, hooks, &pm->map, e, phys, timing);
            check_write_entry(hooks, e, phys, &writes);
        }
    }
    assert(writes > 0);
}

static x68k_bus_t *make_bus(mc68k_bus_t *hooks, mc68k_state_t **cpu_out) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b);
    assert(x68k_bus_set_local_memory(b, LOCAL_MB));
    fill_memory(b);
    *hooks = x68k_bus_get_hooks(b);
    *cpu_out = mc68k_create_model(hooks, MC68K_MODEL_68060);
    assert(*cpu_out);
    x68k_bus_set_cpu(b, *cpu_out);
    assert(x68k_bus_set_timing_profile(b, X68K_TIMING_PROFILE_060TURBO));
    mc68k_reset(
        *cpu_out); /* スーパーバイザにする（突き合わせのコールバックが保護で失敗しない） */
    return b;
}

static void test_all_pages_match_callbacks(void) {
    mc68k_bus_t hooks;
    mc68k_state_t *cpu;
    x68k_bus_t *b = make_bus(&hooks, &cpu);
    assert(hooks.page_map == &b->pmap->map);
    unsigned direct, timing;
    check_all(b, &hooks, &direct, &timing);
    assert(direct > 0 && timing > 0);
    /* RAM、ROM、ローカルメモリのページが載っている */
    assert(b->pmap->main[0].flags & MC68K_PAGE_READ_DIRECT);
    assert(b->pmap->main[0xFF0].flags & MC68K_PAGE_READ_DIRECT);
    assert(b->pmap->local_slots == 1);
    assert(b->pmap->local[0].flags & MC68K_PAGE_READ_DIRECT);
    /* GVRAM は載る（読み書きとも素の配列）。MMIO・TVRAM はコールバックのまま */
    assert(b->pmap->main[0xC00].flags & MC68K_PAGE_READ_DIRECT);
    assert(b->pmap->main[0xDFF].flags & MC68K_PAGE_READ_DIRECT);
    assert(!(b->pmap->main[0xE00].flags & MC68K_PAGE_READ_DIRECT));
    assert(!(b->pmap->main[0xE88].flags & MC68K_PAGE_READ_DIRECT));
    assert(!(b->pmap->main[0xE90].flags & MC68K_PAGE_READ_DIRECT));
    /* SCSI ROM 窓 $FC0000-$FDFFFF は載らない */
    assert(!(b->pmap->main[0xFC0].flags & MC68K_PAGE_READ_DIRECT));
    assert(!(b->pmap->main[0xFD0].flags & MC68K_PAGE_READ_DIRECT));
    /* 24bit ミラー: 0x01000000 は 0 と同じ 2 段目 */
    assert(b->pmap->map.l1[1] == b->pmap->main);
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

static void test_triggers_rebuild_table(void) {
    mc68k_bus_t hooks;
    mc68k_state_t *cpu;
    x68k_bus_t *b = make_bus(&hooks, &cpu);
    unsigned direct, timing;

    /* ram_limit（SRAM $ED0008）: 4KB 境界でない値は境界ページを載せない */
    const uint32_t limits[] = {0x400000u, 0xC00000u, 0x4010A0u, 0x100000u};
    for (unsigned i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        set_ram_limit(b, limits[i]);
        check_all(b, &hooks, &direct, &timing);
    }
    set_ram_limit(b, 0x4010A0u);
    assert(!(b->pmap->main[0x401].flags & MC68K_PAGE_READ_DIRECT)); /* 境界ページ */
    assert(b->pmap->main[0x400].flags & MC68K_PAGE_READ_DIRECT);
    set_ram_limit(b, 0xC00000u);

    /* ウェイトポート $E8E009（RAM/ROM のウェイトが変わる） */
    const uint8_t ports[] = {0x00u, 0x0Fu, 0xA5u, 0xFFu};
    for (unsigned i = 0; i < sizeof ports; i++) {
        x68k_bus_write8(b, WAIT_PORT_ADDR, ports[i]);
        check_all(b, &hooks, &direct, &timing);
    }

    /* timing profile */
    for (int p = 0; p < X68K_TIMING_PROFILE_COUNT; p++) {
        assert(x68k_bus_set_timing_profile(b, (x68k_timing_profile_t)p));
        check_all(b, &hooks, &direct, &timing);
    }
    assert(x68k_bus_set_timing_profile(b, X68K_TIMING_PROFILE_X68030));

    /* ユーザモード保護とエリアセット・拡張エリア */
    x68k_bus_set_super_protect(b, 1);
    check_all(b, &hooks, &direct, &timing);
    assert(b->pmap->main[0].flags & MC68K_PAGE_SUPERVISOR_ONLY);     /* 低位 RAM */
    assert(b->pmap->main[0xFF0].flags & MC68K_PAGE_SUPERVISOR_ONLY); /* ROM */
    for (unsigned v = 0; v < 8; v++) {
        x68k_bus_write8(b, AREA_SET_ADDR, (uint8_t)(v * 37u));
        check_all(b, &hooks, &direct, &timing);
    }
    for (unsigned i = 0; i < 5; i++) {
        x68k_bus_write8(b, EXT_AREA_ADDR + 2u * i, (uint8_t)(0x5Au + i));
        check_all(b, &hooks, &direct, &timing);
    }
    x68k_bus_set_super_protect(b, 0);
    check_all(b, &hooks, &direct, &timing);
    assert(!(b->pmap->main[0].flags & MC68K_PAGE_SUPERVISOR_ONLY));

    /* RESET 命令（ウェイトポートと DMAC を初期化する） */
    x68k_bus_reset_peripheral(b);
    check_all(b, &hooks, &direct, &timing);

    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

static void test_dmac_disable_mask(void) {
    mc68k_bus_t hooks;
    mc68k_state_t *cpu;
    x68k_bus_t *b = make_bus(&hooks, &cpu);
    assert(b->pmap->map.disable_mask == 0);
    b->dmac.ch2_active = 1;
    x68k_bus_page_map_update_mask(b);
    assert(b->pmap->map.disable_mask == X68K_PAGE_MAP_DISABLE_DMAC);
    b->dmac.ch2_active = 0;
    x68k_bus_page_map_update_mask(b);
    assert(b->pmap->map.disable_mask == 0);
    /* ch2 動作中に refresh しても、無効化ビットは維持される */
    b->dmac.ch2_active = 1;
    x68k_bus_page_map_refresh(b);
    assert(b->pmap->map.disable_mask == X68K_PAGE_MAP_DISABLE_DMAC);
    b->dmac.ch2_active = 0;
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

static void test_write_direct_flags(void) {
    mc68k_bus_t hooks;
    mc68k_state_t *cpu;
    x68k_bus_t *b = make_bus(&hooks, &cpu);
    /* RAM・ローカルメモリ・GVRAM が WRITE_DIRECT。ROM・MMIO・TVRAM・SCSI ROM 窓は付かない。 */
    assert(b->pmap->main[0].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(b->pmap->main[0xFF].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(b->pmap->local[0].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(!(b->pmap->main[0xFF0].flags & MC68K_PAGE_WRITE_DIRECT)); /* ROM */
    assert(!(b->pmap->main[0xE88].flags & MC68K_PAGE_WRITE_DIRECT));
    assert(!(b->pmap->main[0xE90].flags & MC68K_PAGE_WRITE_DIRECT));
    assert(b->pmap->main[0xC00].flags & MC68K_PAGE_WRITE_DIRECT); /* GVRAM */
    assert(b->pmap->main[0xDFF].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(!(b->pmap->main[0xE00].flags & MC68K_PAGE_WRITE_DIRECT)); /* TVRAM */
    assert(!(b->pmap->main[0xFC0].flags & MC68K_PAGE_WRITE_DIRECT));
    /* ram_limit の境界ページは載らない */
    set_ram_limit(b, 0x4010A0u);
    assert(!(b->pmap->main[0x401].flags & MC68K_PAGE_WRITE_DIRECT));
    assert(b->pmap->main[0x400].flags & MC68K_PAGE_WRITE_DIRECT);
    /* ユーザモード保護: 低位 RAM は SUPERVISOR_ONLY 付きで WRITE_DIRECT のまま */
    set_ram_limit(b, 0xC00000u);
    x68k_bus_set_super_protect(b, 1);
    assert(b->pmap->main[0].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(b->pmap->main[0].flags & MC68K_PAGE_SUPERVISOR_ONLY);
    /* GVRAM 以降は保護中 SUPERVISOR_ONLY 付きで WRITE_DIRECT のまま */
    assert(b->pmap->main[0xC00].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(b->pmap->main[0xC00].flags & MC68K_PAGE_SUPERVISOR_ONLY);
    assert(b->pmap->main[0xDFF].flags & MC68K_PAGE_SUPERVISOR_ONLY);
    unsigned direct, timing;
    check_all(b, &hooks, &direct, &timing);
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

static void test_ram_watch_rebuilds_table(void) {
    mc68k_bus_t hooks;
    mc68k_state_t *cpu;
    x68k_bus_t *b = make_bus(&hooks, &cpu);
    unsigned direct, timing;
    const unsigned all = count_write_direct(b->pmap);
    /* 範囲と重なるメイン RAM のページだけ WRITE_DIRECT が外れる（読み出しは直接のまま）。 */
    x68k_bus_set_ram_watch(b, 0x1FF0u, 0x20u); /* ページ 1 と 2 にまたがる */
    assert(x68k_bus_page_map_verify(b));
    assert(!(b->pmap->main[1].flags & MC68K_PAGE_WRITE_DIRECT));
    assert(!(b->pmap->main[2].flags & MC68K_PAGE_WRITE_DIRECT));
    assert(b->pmap->main[1].flags & MC68K_PAGE_READ_DIRECT);
    assert(b->pmap->main[0].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(b->pmap->main[3].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(b->pmap->local[0].flags & MC68K_PAGE_WRITE_DIRECT); /* ローカルメモリは対象外 */
    assert(count_write_direct(b->pmap) == all - 2u);
    check_all(b, &hooks, &direct, &timing);
    /* ページ境界ぴったり: 範囲が次のページへ 1 バイトも入らなければ重ならない。 */
    x68k_bus_set_ram_watch(b, 0x3000u, 0x1000u);
    assert(!(b->pmap->main[3].flags & MC68K_PAGE_WRITE_DIRECT));
    assert(b->pmap->main[2].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(b->pmap->main[4].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(count_write_direct(b->pmap) == all - 1u);
    /* 範囲が GVRAM に重なっても GVRAM は WRITE_DIRECT のまま（write8 の GVRAM は監視しない）。
     */
    x68k_bus_set_ram_watch(b, 0xBFF000u, 0x4000u);
    assert(x68k_bus_page_map_verify(b));
    assert(b->pmap->main[0xC00].flags & MC68K_PAGE_WRITE_DIRECT);
    assert(b->pmap->main[0xC02].flags & MC68K_PAGE_WRITE_DIRECT);
    check_all(b, &hooks, &direct, &timing);
    /* 解除すると全ページが戻る。 */
    x68k_bus_set_ram_watch(b, 0u, 0u);
    assert(x68k_bus_page_map_verify(b));
    assert(count_write_direct(b->pmap) == all);
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

/* DMAC ch2 のロック違反で破棄した続きバイトの記憶は、その書き込みアクセスの中で閉じる。
 * 表の直接経路は次のアクセス開始時の同期を通らないため、アクセス終了時に解除する。 */
static void test_dmac_discard_cleared_at_access_end(void) {
    mc68k_bus_t hooks;
    mc68k_state_t *cpu;
    x68k_bus_t *b = make_bus(&hooks, &cpu);
    const uint32_t mar = 0xE84080u + 0x0Cu;
    b->dmac.ch2_active = 1;
    x68k_bus_page_map_update_mask(b);
    assert(hooks.write_word(hooks.ctx, mar, 0x1234u)); /* ロック違反: 上位を破棄、続きも破棄 */
    assert(b->dmac.ch2_active == 0);
    assert(b->dmac.ch2_discard_left == 0);
    b->dmac.ch2_active = 1;
    assert(hooks.write_byte(hooks.ctx, mar, 0x12u));
    assert(b->dmac.ch2_discard_left == 0);
    b->dmac.ch2_active = 1;
    assert(hooks.write_long(hooks.ctx, mar, 0x12345678u));
    assert(b->dmac.ch2_discard_left == 0);
    b->dmac.ch2_active = 0;
    x68k_bus_page_map_update_mask(b);
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

static void test_snapshot_restore_rebuilds(void) {
    mc68k_bus_t hooks;
    mc68k_state_t *cpu;
    x68k_bus_t *b = make_bus(&hooks, &cpu);
    x68k_page_map_t *pmap = b->pmap;
    x68k_bus_guest_clock_set_paused(b, 1);
    unsigned direct, timing;
    set_ram_limit(b, 0x800000u);
    x68k_snapshot_status_t status;
    x68k_bus_snapshot_t *snap = x68k_bus_snapshot_create(b, &status);
    assert(snap);
    set_ram_limit(b, 0x200000u); /* 保存後に状態を変える */
    check_all(b, &hooks, &direct, &timing);
    x68k_bus_restore_plan_t *plan = x68k_bus_snapshot_prepare(b, snap, &status);
    assert(plan);
    x68k_bus_snapshot_commit(b, snap, plan);
    assert(b->pmap == pmap); /* 表の実体は復元で上書きされない */
    check_all(b, &hooks, &direct, &timing);
    x68k_bus_snapshot_destroy(snap);
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
}

int main(void) {
    test_all_pages_match_callbacks();
    test_triggers_rebuild_table();
    test_dmac_disable_mask();
    test_write_direct_flags();
    test_ram_watch_rebuilds_table();
    test_dmac_discard_cleared_at_access_end();
    test_snapshot_restore_rebuilds();
    printf("page_map_test: OK\n");
    return 0;
}
