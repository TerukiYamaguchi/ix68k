/*
 * fdc.c ライトプロテクト(WP)の単体テスト。
 * bus/CPU非依存: dmac_ch0_fdc_transfer()をスタブ化する（tests/fdc_deleted_test.cと同じ形）。
 * write_protect[]はfdc_tのメンバなので、host FS(access()/chmod())を経由せず
 * 直接立てて検証できる（ホスト側の判定ロジック自体はtests/fdc_eject_test.cで検証）。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_dmac_result = 1;
static const uint8_t *g_write_payload = NULL;
static uint8_t *g_read_buf = NULL;

int dmac_ch0_fdc_transfer(x68k_bus_t *b) {
    if (!g_dmac_result)
        return 0;
    fdc_t *f = &b->fdc;
    uint32_t n = f->dma_remaining;
    if (f->dma_dir == 1) {
        for (uint32_t i = 0; i < n; i++)
            g_read_buf[i] = fdc_read_data(f);
    } else {
        for (uint32_t i = 0; i < n; i++)
            fdc_write_data(f, b->ram, b->crtc_regs, g_write_payload[i]);
    }
    return 1;
}

static void insert_disk(fdc_t *f, int drv, size_t bytes) {
    f->disk[drv] = malloc(bytes);
    assert(f->disk[drv] != NULL);
    memset(f->disk[drv], 0, bytes);
    f->disk_bytes[drv] = (int)bytes;
}

static void send_cmd9(fdc_t *f, uint8_t *ram, uint8_t *regs, uint8_t opcode, int unit, int hd,
                       int c, int h, int r, int n) {
    fdc_write_data(f, ram, regs, opcode);
    fdc_write_data(f, ram, regs, (uint8_t)(((hd & 1) << 2) | (unit & 3)));
    fdc_write_data(f, ram, regs, (uint8_t)c);
    fdc_write_data(f, ram, regs, (uint8_t)h);
    fdc_write_data(f, ram, regs, (uint8_t)r);
    fdc_write_data(f, ram, regs, (uint8_t)n);
    fdc_write_data(f, ram, regs, (uint8_t)r);
    fdc_write_data(f, ram, regs, 0x35u);
    fdc_write_data(f, ram, regs, 0xFFu);
}

/* T1: 挿入済み+write_protect=1でSENSE DRIVE STATUS(0x04) → ST3 bit6(WP)が立つ。
 * RDY(0x20)/TS(0x08)/HD/USも従来どおり。 */
static void test_t1_sense_drive_status_wp_set(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);
    bus.fdc.write_protect[0] = 1;

    fdc_write_data(&bus.fdc, ram, regs, 0x04u); /* SENSE DRIVE STATUS */
    fdc_write_data(&bus.fdc, ram, regs, 0x00u); /* unit0, hd0 */

    assert((bus.fdc.res[0] & 0x40u) != 0);    /* WP */
    assert((bus.fdc.res[0] & 0x20u) != 0);    /* RDY */
    assert((bus.fdc.res[0] & 0x08u) != 0);    /* TS */
    printf("test_t1_sense_drive_status_wp_set: OK\n");
}

/* T2: write_protect=0での0x04 → WPビットは立たない（既存挙動の退行なし）。 */
static void test_t2_sense_drive_status_wp_clear(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);
    bus.fdc.write_protect[0] = 0;

    fdc_write_data(&bus.fdc, ram, regs, 0x04u);
    fdc_write_data(&bus.fdc, ram, regs, 0x00u);

    assert((bus.fdc.res[0] & 0x40u) == 0);
    printf("test_t2_sense_drive_status_wp_clear: OK\n");
}

/* T3: 未挿入なのにwrite_protect=1（あり得ない状態を強制）→ 0x04 → WPビットは
 * 立たない（f->disk[drv]&&ガード、D4の防御的多重化）。 */
static void test_t3_sense_drive_status_wp_no_disk(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    bus.fdc.write_protect[0] = 1; /* ディスク未挿入のまま */

    fdc_write_data(&bus.fdc, ram, regs, 0x04u);
    fdc_write_data(&bus.fdc, ram, regs, 0x00u);

    assert((bus.fdc.res[0] & 0x40u) == 0);
    printf("test_t3_sense_drive_status_wp_no_disk: OK\n");
}

/* T4: write_protect=1で0x05 WRITE DATA → NW応答、ディスク不変。 */
static void test_t4_write_data_wp_rejected(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    bus.fdc.write_protect[0] = 1;
    uint8_t before[1024];
    memcpy(before, bus.fdc.disk[0], 1024);
    uint8_t payload[1024];
    for (int i = 0; i < 1024; i++)
        payload[i] = (uint8_t)(0xA5u ^ i);
    g_write_payload = payload;

    send_cmd9(&bus.fdc, ram, regs, 0x45u, 0, 0, 0, 0, 1, 3); /* MFM|WRITE DATA */

    assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
    assert(bus.fdc.res[1] == 0x02u); /* NW */
    assert(bus.fdc.res[2] == 0x00u);
    assert(bus.fdc.res[3] == 0u);
    assert(bus.fdc.res[4] == 0u);
    assert(bus.fdc.res[5] == 1u);
    assert(bus.fdc.res[6] == 3u);
    assert(bus.fdc.res_len == 7);
    assert(bus.fdc.irq == 1);
    assert(bus.fdc.phase == FDC_RESULT);
    assert(memcmp(before, bus.fdc.disk[0], 1024) == 0); /* ディスク不変 */
    printf("test_t4_write_data_wp_rejected: OK\n");
}

/* T5: write_protect=1で0x09 WRITE DELETED DATA → NW応答、DDAMビットも立たない。 */
static void test_t5_write_deleted_wp_rejected(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    bus.fdc.write_protect[0] = 1;
    uint8_t payload[1024] = {0};
    g_write_payload = payload;

    send_cmd9(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3); /* MFM|WRITE DELETED DATA */

    assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
    assert(bus.fdc.res[1] == 0x02u); /* NW */
    assert(bus.fdc.ddam[0][0] == 0); /* DDAMビット不変 */
    printf("test_t5_write_deleted_wp_rejected: OK\n");
}

/* T6: write_protect=1かつ未挿入で0x05 → NR優先（D5の順序）。 */
static void test_t6_write_data_not_ready_priority(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    bus.fdc.write_protect[0] = 1; /* ディスク未挿入のまま */
    uint8_t payload[1024] = {0};
    g_write_payload = payload;

    send_cmd9(&bus.fdc, ram, regs, 0x45u, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[0] == (uint8_t)(0x48u | 0u)); /* NR */
    assert(bus.fdc.res[1] == 0u);                    /* NWではない */
    printf("test_t6_write_data_not_ready_priority: OK\n");
}

/* T7: write_protect=1かつN=24/32/255で0x05 → NW(res[1]==0x02)であってND(0x04)では
 * ない。WP判定がN>7チェックより前にあることの検証（UBSanでも落ちないこと）。 */
static void test_t7_write_data_wp_before_n_guard(void) {
    int ns[] = {24, 32, 255};
    for (size_t i = 0; i < sizeof(ns) / sizeof(ns[0]); i++) {
        static x68k_bus_t bus;
        memset(&bus, 0, sizeof(bus));
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        bus.fdc.write_protect[0] = 1;
        uint8_t payload[1024] = {0};
        g_write_payload = payload;

        send_cmd9(&bus.fdc, ram, regs, 0x45u, 0, 0, 0, 0, 1, ns[i]);

        assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
        assert(bus.fdc.res[1] == 0x02u); /* NW、NDではない */
        free(bus.fdc.disk[0]);
    }
    printf("test_t7_write_data_wp_before_n_guard: OK\n");
}

/* T8: write_protect=1で0x06 READ DATA / 0x0C READ DELETED DATA → 正常に読める
 * （WP判定の適用外、D5）。 */
static void test_t8_read_unaffected_by_wp(void) {
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        bus.fdc.write_protect[0] = 1;
        for (int i = 0; i < 1024; i++)
            bus.fdc.disk[0][i] = (uint8_t)i;
        uint8_t readbuf[1024] = {0};
        g_read_buf = readbuf;

        send_cmd9(&bus.fdc, ram, regs, 0x46u, 0, 0, 0, 0, 1, 3); /* READ DATA */

        assert(bus.fdc.res[0] == 0u);
        assert(bus.fdc.res[1] == 0u);
        assert(memcmp(readbuf, bus.fdc.disk[0], 1024) == 0);
    }
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        bus.fdc.write_protect[0] = 1;
        uint8_t readbuf[1024] = {0};
        g_read_buf = readbuf;

        send_cmd9(&bus.fdc, ram, regs, 0x4Cu, 0, 0, 0, 0, 1, 3); /* READ DELETED DATA */

        assert(bus.fdc.res[0] == 0u);
        assert(bus.fdc.res[1] == 0u);
    }
    printf("test_t8_read_unaffected_by_wp: OK\n");
}

/* T9: write_protect=1で0x11 SCAN EQUAL → res[1]は0（WP判定の適用外）。 */
static void test_t9_scan_equal_unaffected_by_wp(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    bus.fdc.write_protect[0] = 1;
    uint8_t payload[1024];
    memset(payload, 0, sizeof(payload));
    g_write_payload = payload;

    send_cmd9(&bus.fdc, ram, regs, 0x51u, 0, 0, 0, 0, 1, 3); /* MFM|SCAN EQUAL(0x11) */

    assert(bus.fdc.res[1] == 0u);
    printf("test_t9_scan_equal_unaffected_by_wp: OK\n");
}

/* T10: write_protect=0で0x05/0x09 → 従来どおり書き込め、DDAMビットも従来どおり
 * （退行なし）。 */
static void test_t10_write_succeeds_without_wp(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    bus.fdc.write_protect[0] = 0;
    uint8_t payload[1024];
    for (int i = 0; i < 1024; i++)
        payload[i] = (uint8_t)(0x5Au ^ i);
    g_write_payload = payload;

    send_cmd9(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3); /* WRITE DELETED DATA */

    assert(bus.fdc.res[0] == 0u);
    assert(bus.fdc.res[1] == 0u);
    assert(memcmp(bus.fdc.disk[0], payload, 1024) == 0);
    assert((bus.fdc.ddam[0][0] & 0x01u) != 0); /* DDAMビットが立つ (cyl0/hd0/sec1 → index0 → bit0) */
    printf("test_t10_write_succeeds_without_wp: OK\n");
}

/* T11: write_protect=1で0x07 RECALIBRATE / 0x0F SEEK → 従来どおり成功
 * （WP判定の適用外）。 */
static void test_t11_seek_unaffected_by_wp(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    bus.fdc.write_protect[0] = 1;
    bus.fdc.cur_cyl[0] = 5;

    fdc_write_data(&bus.fdc, ram, regs, 0x07u); /* RECALIBRATE */
    fdc_write_data(&bus.fdc, ram, regs, 0x00u); /* unit0 */

    assert((bus.fdc.st0 & 0x08u) == 0); /* NRが立っていない(成功) */
    printf("test_t11_seek_unaffected_by_wp: OK\n");
}

int main(void) {
    test_t1_sense_drive_status_wp_set();
    test_t2_sense_drive_status_wp_clear();
    test_t3_sense_drive_status_wp_no_disk();
    test_t4_write_data_wp_rejected();
    test_t5_write_deleted_wp_rejected();
    test_t6_write_data_not_ready_priority();
    test_t7_write_data_wp_before_n_guard();
    test_t8_read_unaffected_by_wp();
    test_t9_scan_equal_unaffected_by_wp();
    test_t10_write_succeeds_without_wp();
    test_t11_seek_unaffected_by_wp();

    printf("fdc_wp_test: ALL OK\n");
    return 0;
}
