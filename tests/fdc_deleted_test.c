/*
 * fdc.c WRITE/READ DELETED DATA(opcode 0x09/0x0C)の単体テスト
 * （IOCS _B_WRITED(0x49)/_B_READDL(0x4C)用）。
 * bus/CPU非依存: dmac_ch0_fdc_transfer()をスタブ化する（tests/fdc_scan_test.cと同じ形）。
 *
 * READ方向(dma_dir==1)とWRITE方向(dma_dir==0)の両方をこのスタブでドレインする。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_dmac_result = 1;
static const uint8_t *g_write_payload = NULL; /* WRITE(0x05/0x09)がホストから受け取るデータ */
static uint8_t *g_read_buf = NULL;             /* READ(0x06/0x0C)がホストへ供給する先 */

int dmac_ch0_fdc_transfer(x68k_bus_t *b) {
    if (!g_dmac_result)
        return 0; /* T16: 非武装をシミュレートし、fdc.c側のMARフォールバックへ委ねる */
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

/* WRITE系(0x05/0x09)コマンド9バイトを送る。byte6(EOT)=R, byte7(GPL)=$35,
 * byte8(DTL)=$FFはROM実測どおりだがfdc.c側では未使用。 */
static void send_write(fdc_t *f, uint8_t *ram, uint8_t *regs, uint8_t opcode, int unit, int hd,
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

/* READ系(0x06/0x0C)コマンド9バイトを送る。 */
static void send_read(fdc_t *f, uint8_t *ram, uint8_t *regs, uint8_t opcode, int unit, int hd,
                       int c, int h, int r, int n) {
    send_write(f, ram, regs, opcode, unit, hd, c, h, r, n);
}

/* T1: 0x09(WRITE DELETED DATA) byte0送出直後にcmd_total==9でFDC_CMD状態。 */
static void test_t1_write_deleted_command_length(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);

    fdc_write_data(&bus.fdc, ram, regs, 0xC9u); /* MT|MFM|WRITE DELETED DATA(0x09) */
    assert(bus.fdc.cmd_total == 9);
    assert(bus.fdc.phase == FDC_CMD);
    printf("test_t1_write_deleted_command_length: OK\n");
}

/* T2: 0x0C(READ DELETED DATA) byte0送出直後も同様。 */
static void test_t2_read_deleted_command_length(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);

    fdc_write_data(&bus.fdc, ram, regs, 0xCCu); /* MT|MFM|READ DELETED DATA(0x0C) */
    assert(bus.fdc.cmd_total == 9);
    assert(bus.fdc.phase == FDC_CMD);
    printf("test_t2_read_deleted_command_length: OK\n");
}

/* T3: 0x09正常書き込み → ディスク内容更新、DDAMビット=1、CM無し、R=sec+1、irq=1。 */
static void test_t3_write_deleted_normal(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    uint8_t payload[1024];
    for (int i = 0; i < 1024; i++)
        payload[i] = (uint8_t)(0x5Au ^ i);
    g_write_payload = payload;

    send_write(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3); /* MFM|WRITE DELETED DATA */

    assert(memcmp(bus.fdc.disk[0], payload, 1024) == 0);
    assert(bus.fdc.res[0] == 0u);
    assert(bus.fdc.res[1] == 0u);
    assert(bus.fdc.res[2] == 0u);
    assert(bus.fdc.res[5] == 2u); /* sec+1 */
    assert(bus.fdc.irq == 1);
    printf("test_t3_write_deleted_normal: OK\n");
}

/* T4: T3の直後に0x0Cで同セクタ読み出し → データ一致、CM無し、R=sec+1。 */
static void test_t4_read_deleted_after_write_deleted(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    uint8_t payload[1024];
    for (int i = 0; i < 1024; i++)
        payload[i] = (uint8_t)(0x5Au ^ i);
    g_write_payload = payload;
    send_write(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3);
    while (bus.fdc.phase == FDC_RESULT)
        fdc_read_data(&bus.fdc);

    uint8_t readbuf[1024] = {0};
    g_read_buf = readbuf;
    send_read(&bus.fdc, ram, regs, 0x4Cu, 0, 0, 0, 0, 1, 3);

    assert(memcmp(readbuf, payload, 1024) == 0);
    assert(bus.fdc.res[2] == 0x00u); /* CM無し */
    assert(bus.fdc.res[5] == 2u);    /* sec+1 */
    printf("test_t4_read_deleted_after_write_deleted: OK\n");
}

/* T5: 素のセクタ(通常DAM)を0x0Cで読む → データは転送される、CM、R=sec。 */
static void test_t5_read_deleted_on_plain_sector(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    for (int i = 0; i < 1024; i++)
        bus.fdc.disk[0][i] = (uint8_t)i;

    uint8_t readbuf[1024] = {0};
    g_read_buf = readbuf;
    send_read(&bus.fdc, ram, regs, 0x4Cu, 0, 0, 0, 0, 1, 3);

    assert(memcmp(readbuf, bus.fdc.disk[0], 1024) == 0); /* データは転送される */
    assert(bus.fdc.res[2] == 0x40u);                     /* CM */
    assert(bus.fdc.res[5] == 1u);                        /* sec そのまま */
    printf("test_t5_read_deleted_on_plain_sector: OK\n");
}

/* T6: T3の後0x06(通常READ)で読む → データは転送される、CM、R=sec（D3-2）。 */
static void test_t6_read_normal_on_deleted_sector(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    uint8_t payload[1024];
    for (int i = 0; i < 1024; i++)
        payload[i] = (uint8_t)(0x5Au ^ i);
    g_write_payload = payload;
    send_write(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3);
    while (bus.fdc.phase == FDC_RESULT)
        fdc_read_data(&bus.fdc);

    uint8_t readbuf[1024] = {0};
    g_read_buf = readbuf;
    send_read(&bus.fdc, ram, regs, 0x46u, 0, 0, 0, 0, 1, 3); /* MFM|READ DATA(0x06) */

    assert(memcmp(readbuf, payload, 1024) == 0);
    assert(bus.fdc.res[2] == 0x40u); /* CM */
    assert(bus.fdc.res[5] == 1u);    /* sec そのまま */
    printf("test_t6_read_normal_on_deleted_sector: OK\n");
}

/* T7: T3の後0x05(通常WRITE)→0x06 → CM無し、R=sec+1（DDAMビットがクリアされた）。 */
static void test_t7_write_normal_clears_ddam(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    uint8_t deleted_payload[1024];
    for (int i = 0; i < 1024; i++)
        deleted_payload[i] = (uint8_t)(0x5Au ^ i);
    g_write_payload = deleted_payload;
    send_write(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3);
    while (bus.fdc.phase == FDC_RESULT)
        fdc_read_data(&bus.fdc);

    uint8_t normal_payload[1024];
    for (int i = 0; i < 1024; i++)
        normal_payload[i] = (uint8_t)i;
    g_write_payload = normal_payload;
    send_write(&bus.fdc, ram, regs, 0x45u, 0, 0, 0, 0, 1, 3); /* MFM|WRITE DATA(0x05) */
    while (bus.fdc.phase == FDC_RESULT)
        fdc_read_data(&bus.fdc);

    uint8_t readbuf[1024] = {0};
    g_read_buf = readbuf;
    send_read(&bus.fdc, ram, regs, 0x46u, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[2] == 0x00u); /* CM無し: DDAMビットがクリアされた */
    assert(bus.fdc.res[5] == 2u);    /* sec+1 */
    printf("test_t7_write_normal_clears_ddam: OK\n");
}

/* T8: T3の後0x05(通常WRITE)→0x0C → CM、R=sec（0x0C視点では通常DAMになった）。 */
static void test_t8_write_normal_then_read_deleted(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    uint8_t deleted_payload[1024];
    for (int i = 0; i < 1024; i++)
        deleted_payload[i] = (uint8_t)(0x5Au ^ i);
    g_write_payload = deleted_payload;
    send_write(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3);
    while (bus.fdc.phase == FDC_RESULT)
        fdc_read_data(&bus.fdc);

    uint8_t normal_payload[1024];
    for (int i = 0; i < 1024; i++)
        normal_payload[i] = (uint8_t)i;
    g_write_payload = normal_payload;
    send_write(&bus.fdc, ram, regs, 0x45u, 0, 0, 0, 0, 1, 3);
    while (bus.fdc.phase == FDC_RESULT)
        fdc_read_data(&bus.fdc);

    uint8_t readbuf[1024] = {0};
    g_read_buf = readbuf;
    send_read(&bus.fdc, ram, regs, 0x4Cu, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[2] == 0x40u); /* CM */
    assert(bus.fdc.res[5] == 1u);    /* sec そのまま */
    printf("test_t8_write_normal_then_read_deleted: OK\n");
}

/* T9: メディアなしで0x09 → NR、ST1/ST2=0、irq=1。 */
static void test_t9_write_deleted_not_ready(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};

    send_write(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[0] == (uint8_t)(0x48u | 0u));
    assert(bus.fdc.res[1] == 0u);
    assert(bus.fdc.res[2] == 0u);
    assert(bus.fdc.irq == 1);
    printf("test_t9_write_deleted_not_ready: OK\n");
}

/* T10: メディアなしで0x0C → 同上。 */
static void test_t10_read_deleted_not_ready(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};

    send_read(&bus.fdc, ram, regs, 0x4Cu, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[0] == (uint8_t)(0x48u | 0u));
    assert(bus.fdc.res[1] == 0u);
    assert(bus.fdc.res[2] == 0u);
    assert(bus.fdc.irq == 1);
    printf("test_t10_read_deleted_not_ready: OK\n");
}

/* T11: 0x09境界値。N=24/32/255、sec==0/9、cyl==77、hd!=head は
 * すべてAT|NDで拒否、ディスク不変・DDAMビット不変・irq=1（UBSan/ASanでも
 * 落ちないこと。D4）。 */
static void test_t11_write_deleted_boundary(void) {
    struct {
        int n, sec, cyl, hd, head;
    } cases[] = {
        {24, 1, 0, 0, 0}, {32, 1, 0, 0, 0}, {255, 1, 0, 0, 0}, {3, 0, 0, 0, 0},
        {3, 9, 0, 0, 0},  {3, 1, 77, 0, 0}, {3, 1, 0, 1, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        static x68k_bus_t bus;
        memset(&bus, 0, sizeof(bus));
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        uint8_t before[1024];
        memcpy(before, bus.fdc.disk[0], 1024);
        uint8_t payload[1024] = {0};
        g_write_payload = payload;

        send_write(&bus.fdc, ram, regs, 0x49u, 0, cases[i].hd, cases[i].cyl, cases[i].head,
                   cases[i].sec, cases[i].n);

        assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
        assert(bus.fdc.res[1] == 0x04u);
        assert(bus.fdc.irq == 1);
        assert(memcmp(before, bus.fdc.disk[0], 1024) == 0); /* ディスク不変 */
        assert(bus.fdc.ddam[0][0] == 0);                    /* DDAMビット不変 */
        free(bus.fdc.disk[0]);
    }
    printf("test_t11_write_deleted_boundary: OK\n");
}

/* T12: 0x0C同上の境界値。 */
static void test_t12_read_deleted_boundary(void) {
    struct {
        int n, sec, cyl, hd, head;
    } cases[] = {
        {24, 1, 0, 0, 0}, {32, 1, 0, 0, 0}, {255, 1, 0, 0, 0}, {3, 0, 0, 0, 0},
        {3, 9, 0, 0, 0},  {3, 1, 77, 0, 0}, {3, 1, 0, 1, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        static x68k_bus_t bus;
        memset(&bus, 0, sizeof(bus));
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        uint8_t before[1024];
        memcpy(before, bus.fdc.disk[0], 1024);
        uint8_t readbuf[1024] = {0};
        g_read_buf = readbuf;

        send_read(&bus.fdc, ram, regs, 0x4Cu, 0, cases[i].hd, cases[i].cyl, cases[i].head,
                  cases[i].sec, cases[i].n);

        assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
        assert(bus.fdc.res[1] == 0x04u);
        assert(bus.fdc.irq == 1);
        assert(memcmp(before, bus.fdc.disk[0], 1024) == 0);
        free(bus.fdc.disk[0]);
    }
    printf("test_t12_read_deleted_boundary: OK\n");
}

/* T13: 最終セクタ(cyl=76,hd=1,sec=8) → 正常終了、DDAMインデックス1231(範囲内)。 */
static void test_t13_write_deleted_last_sector(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);
    uint8_t payload[1024] = {0};
    g_write_payload = payload;

    send_write(&bus.fdc, ram, regs, 0x49u, 0, 1, 76, 1, 8, 3);

    assert(bus.fdc.res[0] == 0u);
    assert(bus.fdc.res[1] == 0u);
    /* index = (76*2+1)*8 + (8-1) = 153*8+7 = 1231 → byte 153, bit 7 */
    assert((bus.fdc.ddam[0][153] & 0x80u) != 0);
    printf("test_t13_write_deleted_last_sector: OK\n");
}

/* T16: DMAC非武装+MAR範囲外での0x09 → ハングせずRESULTフェーズへ到達しirq=1。 */
static void test_t16_write_deleted_mar_invalid_no_hang(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    static uint8_t regs[0x10000] = {0};
    insert_disk(&bus.fdc, 0, 1261568);

    g_dmac_result = 0; /* 非武装 */
    regs[0x400Du] = 0xFFu;
    regs[0x400Eu] = 0xFFu;
    regs[0x400Fu] = 0xFFu; /* MAR範囲外 */

    send_write(&bus.fdc, ram, regs, 0x49u, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.phase == FDC_RESULT);
    assert(bus.fdc.irq == 1);

    g_dmac_result = 1;
    printf("test_t16_write_deleted_mar_invalid_no_hang: OK\n");
}

/* T17: 0x05/0x06にN=24/32/255 → N>7ガードでAT|ND、UB無し。N=0..7は
 * 従来どおりの応答(既存挙動の退行が無いこと。D4)。 */
static void test_t17_normal_rw_n_guard(void) {
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        uint8_t payload[1024] = {0};
        g_write_payload = payload;
        send_write(&bus.fdc, ram, regs, 0x45u, 0, 0, 0, 0, 1, 24); /* N=24 */
        assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
        assert(bus.fdc.res[1] == 0x04u);
    }
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        uint8_t readbuf[1024] = {0};
        g_read_buf = readbuf;
        send_read(&bus.fdc, ram, regs, 0x46u, 0, 0, 0, 0, 1, 32); /* N=32 */
        assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
        assert(bus.fdc.res[1] == 0x04u);
    }
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        uint8_t readbuf[1024] = {0};
        g_read_buf = readbuf;
        send_read(&bus.fdc, ram, regs, 0x46u, 0, 0, 0, 0, 1, 255); /* N=255 */
        assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
        assert(bus.fdc.res[1] == 0x04u);
    }
    /* N=0..7の既存挙動が変わらないこと(N=0, 128バイト転送で正常終了)。 */
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        uint8_t readbuf[128] = {0};
        g_read_buf = readbuf;
        send_read(&bus.fdc, ram, regs, 0x46u, 0, 0, 0, 0, 1, 0); /* N=0 */
        assert(bus.fdc.res[0] == 0u);
        assert(bus.fdc.res[1] == 0u);
    }
    printf("test_t17_normal_rw_n_guard: OK\n");
}

int main(void) {
    test_t1_write_deleted_command_length();
    test_t2_read_deleted_command_length();
    test_t3_write_deleted_normal();
    test_t4_read_deleted_after_write_deleted();
    test_t5_read_deleted_on_plain_sector();
    test_t6_read_normal_on_deleted_sector();
    test_t7_write_normal_clears_ddam();
    test_t8_write_normal_then_read_deleted();
    test_t9_write_deleted_not_ready();
    test_t10_read_deleted_not_ready();
    test_t11_write_deleted_boundary();
    test_t12_read_deleted_boundary();
    test_t13_write_deleted_last_sector();
    test_t16_write_deleted_mar_invalid_no_hang();
    test_t17_normal_rw_n_guard();

    printf("fdc_deleted_test: ALL OK\n");
    return 0;
}
