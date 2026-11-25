/*
 * fdc.c SCAN EQUAL(opcode 0x11)の単体テスト（IOCS _B_VERIFY(0x41)用）。
 * bus/CPU非依存: dmac_ch0_fdc_transfer()をスタブ化する。
 *
 * 注意: fdc_write_data()はコマンド完走時(cmd_idx>=cmd_total)に必ず
 * `f->phase = (f->res_len > 0) ? FDC_RESULT : FDC_IDLE;` でphaseを上書きする
 * （実装側のFDC_CMDアキュムレータ、fdc.c末尾）。実際のDMACエンジンは武装済み
 * なら常にこの時点までに転送を完了させてres_len/phaseを確定させるため、
 * FDC_EXECが外部から観測可能な状態として残ることはない。したがって本スタブは
 * 「呼ばれたら即座にg_dma_payloadを1バイトずつfdc_write_data()へ流し込んで
 * 転送を完走させる」形にする（実DMACの汎用転送エンジンと等価な結果を作る）。
 * これはdma_dir=0(host→FDC、WRITE DATA/SCAN EQUALが共有)方向のみを模擬する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_dmac_result = 1;
static const uint8_t *g_dma_payload = NULL;

int dmac_ch0_fdc_transfer(x68k_bus_t *b) {
    if (!g_dmac_result)
        return 0; /* T20: 非武装をシミュレートし、fdc.c側のMARフォールバックへ委ねる */
    fdc_t *f = &b->fdc;
    uint32_t n = f->dma_remaining;
    for (uint32_t i = 0; i < n; i++)
        fdc_write_data(f, b->ram, b->crtc_regs, g_dma_payload[i]);
    return 1;
}

/* 標準2HD全体(77cyl*2head*8sec*1024byte)等、指定バイト数を確保してdrvへ挿入する。 */
static void insert_disk(fdc_t *f, int drv, size_t bytes) {
    f->disk[drv] = malloc(bytes);
    assert(f->disk[drv] != NULL);
    memset(f->disk[drv], 0, bytes);
    f->disk_bytes[drv] = (int)bytes;
}

/* SCAN EQUALコマンド9バイトを送る。ROM実測($FF8B6A-$FF8B82)どおりの構成だが、
 * byte6(EOT)/byte8(STP)はfdc.c側では読まないため固定値で埋める。9バイト目の
 * 送出でfdc_exec()のcase 0x11が走り、g_dma_payloadが有効ならその場で比較まで
 * 完走する（上のスタブ参照）。 */
static void send_scan(fdc_t *f, uint8_t *ram, uint8_t *regs, int unit, int hd, int c, int h,
                       int r, int n) {
    fdc_write_data(f, ram, regs, 0x51u); /* byte0 = MFM(bit6)|SCAN EQUAL($11) */
    fdc_write_data(f, ram, regs, (uint8_t)(((hd & 1) << 2) | (unit & 3)));
    fdc_write_data(f, ram, regs, (uint8_t)c);
    fdc_write_data(f, ram, regs, (uint8_t)h);
    fdc_write_data(f, ram, regs, (uint8_t)r);
    fdc_write_data(f, ram, regs, (uint8_t)n);
    fdc_write_data(f, ram, regs, (uint8_t)r); /* EOT = R（ROM実測どおり、fdc.cは未使用） */
    fdc_write_data(f, ram, regs, 0x35u);      /* GPL、fdc.cは未使用 */
    fdc_write_data(f, ram, regs, 0xFFu);      /* STP、fdc.cは未使用 */
}

/* T1: 長さテーブル修正の回帰テスト。1バイト目送出直後にcmd_total==9で
 * FDC_CMD状態を保っていること（旧実装はdefault:でcmd_total=1になり、
 * 1バイトだけでInvalid Command応答へ落ちていた）。 */
static void test_t1_command_length(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);

    fdc_write_data(&bus.fdc, ram, regs, 0x51u); /* byte0のみ送出 */
    assert(bus.fdc.cmd_total == 9);
    assert(bus.fdc.phase == FDC_CMD); /* まだ実行されていない */
    printf("test_t1_command_length: OK\n");
}

/* T2: SCAN実行フェーズのDIOは0（fdc_msr()==0x90）。dma_dirを2等へ拡張して
 * いたら壊れる項目。実行中の瞬間は外部から観測不能なため、既存の
 * fdc_eject_test.cと同じ流儀でphaseを直接ポークして式そのものを固定する。 */
static void test_t2_msr_during_exec(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.phase = FDC_EXEC;
    bus.fdc.dma_dir = 0; /* SCAN EQUALはWRITE DATAと同じOCR.DIR=0のまま(D2) */
    assert(fdc_msr(&bus.fdc) == 0x90u);
    printf("test_t2_msr_during_exec: OK\n");
}

/* T2b: 実際にSCAN EQUALを完走させた後もdma_dirが0のままであること
 *（case 0x11がdma_dir=2等へ拡張していないことの実測による裏付け）。 */
static void test_t2b_dma_dir_stays_zero(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);
    uint8_t target[1024] = {0};
    g_dma_payload = target;

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);
    assert(bus.fdc.dma_dir == 0);
    printf("test_t2b_dma_dir_stays_zero: OK\n");
}

/* T3: 一致データ1024バイト → SH、かつディスク内容が書き換わらないこと。 */
static void test_t3_match_sh_no_writeback(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);
    for (int i = 0; i < 1024; i++)
        bus.fdc.disk[0][i] = (uint8_t)(i & 0xFFu);
    uint8_t before[1024];
    memcpy(before, bus.fdc.disk[0], 1024);

    uint8_t target[1024];
    memcpy(target, bus.fdc.disk[0], 1024);
    g_dma_payload = target;

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[2] == 0x08u); /* SH */
    assert(bus.fdc.res[0] == 0u);    /* ST0: NT, unit=0 */
    assert(bus.fdc.res[1] == 0u);
    assert(bus.fdc.res_len == 7);
    assert(bus.fdc.irq == 1);
    assert(bus.fdc.phase == FDC_RESULT);
    assert(memcmp(before, bus.fdc.disk[0], 1024) == 0); /* ディスクは不変 */
    printf("test_t3_match_sh_no_writeback: OK\n");
}

/* T4: 512バイト目だけ不一致 → SN、ディスク内容は不変。 */
static void test_t4_mismatch_sn(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);
    uint8_t before[1024];
    memcpy(before, bus.fdc.disk[0], 1024);

    uint8_t target[1024] = {0};
    target[512] = 0x01u;
    g_dma_payload = target;

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[2] == 0x04u); /* SN */
    assert(memcmp(before, bus.fdc.disk[0], 1024) == 0);
    printf("test_t4_mismatch_sn: OK\n");
}

/* T5: 目的データが全$FF → ディスク内容に関わらずSH（ワイルドカード）。 */
static void test_t5_wildcard_ff(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);
    for (int i = 0; i < 1024; i++)
        bus.fdc.disk[0][i] = (uint8_t)(i & 0xFFu);

    uint8_t target[1024];
    memset(target, 0xFFu, sizeof(target));
    g_dma_payload = target;

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[2] == 0x08u); /* SH */
    printf("test_t5_wildcard_ff: OK\n");
}

/* T6: ディスク側が全$FF、目的データが$00 → SN（ワイルドカードは非対称）。 */
static void test_t6_wildcard_asymmetric(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);
    memset(bus.fdc.disk[0], 0xFFu, 1024);

    uint8_t target[1024] = {0}; /* 全0x00 */
    g_dma_payload = target;

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[2] == 0x04u); /* SN */
    printf("test_t6_wildcard_asymmetric: OK\n");
}

/* T7: 最終バイトだけ不一致 → SN（境界の1バイトも比較されていること）。 */
static void test_t7_last_byte_mismatch(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);

    uint8_t target[1024] = {0};
    target[1023] = 0x01u;
    g_dma_payload = target;

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[2] == 0x04u); /* SN */
    printf("test_t7_last_byte_mismatch: OK\n");
}

/* T8: 結果バイトC/H/R/N。RはREAD/WRITE DATAと異なりsec+1にならない。 */
static void test_t8_result_bytes(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568); /* 標準2HD全体 */

    uint8_t target[1024] = {0};
    g_dma_payload = target;

    send_scan(&bus.fdc, ram, regs, 0, 1, 5, 1, 3, 3);

    assert(bus.fdc.res[3] == 5); /* C */
    assert(bus.fdc.res[4] == 1); /* H */
    assert(bus.fdc.res[5] == 3); /* R（sec+1ではなくsecそのまま） */
    assert(bus.fdc.res[6] == 3); /* N */
    printf("test_t8_result_bytes: OK\n");
}

/* T9: メディア未挿入 → Not Ready(NR)、ST1/ST2は0、実行フェーズに入らない。 */
static void test_t9_not_ready(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    /* insert_diskを呼ばない = disk[0]==NULL */

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[0] == (uint8_t)(0x48u | 0u));
    assert(bus.fdc.res[1] == 0u);
    assert(bus.fdc.res[2] == 0u);
    assert(bus.fdc.res_len == 7);
    assert(bus.fdc.irq == 1);
    printf("test_t9_not_ready: OK\n");
}

/* T10: 有効なジオメトリ(cyl<77,1<=R<=8,N=3,H==HD)だが実ディスクサイズを
 * 超えるセクタを要求 → D11通過後の第二安全網(disk_bytes比較)で範囲外(ND)。 */
static void test_t10_out_of_range_disk_bytes(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 8192); /* 1トラック分(cyl0/head0のみ) */

    send_scan(&bus.fdc, ram, regs, 0, 0, 1, 0, 1, 3); /* cyl=1は幾何学的には有効だが実データ無し */

    assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u)); /* AT */
    assert(bus.fdc.res[1] == 0x04u);                 /* ND */
    printf("test_t10_out_of_range_disk_bytes: OK\n");
}

/* T11: 境界 R=1 / R=8 はいずれも正常にD11/第二安全網を通過し比較まで完走すること。 */
static void test_t11_sector_boundary(void) {
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        uint8_t target[1024] = {0};
        g_dma_payload = target;
        send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);
        assert(bus.fdc.res[1] == 0x00u); /* NDで弾かれていない */
        assert(bus.fdc.res[2] == 0x08u); /* SH: 比較まで完走 */
    }
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        uint8_t target[1024] = {0};
        g_dma_payload = target;
        send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 8, 3);
        assert(bus.fdc.res[1] == 0x00u);
        assert(bus.fdc.res[2] == 0x08u);
    }
    printf("test_t11_sector_boundary: OK\n");
}

/* T12: 直前にWRITE DATAを実行→続けてSCAN EQUAL。dma_cmp/scan_mismatchが
 * 前コマンドから持ち越されないこと（fdc_exec()冒頭クリアの回帰）。 */
static void test_t12_no_carryover_from_write(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);

    uint8_t payload[1024];
    for (int i = 0; i < 1024; i++)
        payload[i] = (uint8_t)i;
    g_dma_payload = payload;

    /* WRITE DATA (0x45=MFM|WRITE DATA) を実行。 */
    fdc_write_data(&bus.fdc, ram, regs, 0x45u);
    fdc_write_data(&bus.fdc, ram, regs, 0x00u); /* unit=0,hd=0 */
    fdc_write_data(&bus.fdc, ram, regs, 0x00u); /* C */
    fdc_write_data(&bus.fdc, ram, regs, 0x00u); /* H */
    fdc_write_data(&bus.fdc, ram, regs, 0x01u); /* R */
    fdc_write_data(&bus.fdc, ram, regs, 0x03u); /* N */
    fdc_write_data(&bus.fdc, ram, regs, 0x08u); /* EOT */
    fdc_write_data(&bus.fdc, ram, regs, 0x35u); /* GPL */
    fdc_write_data(&bus.fdc, ram, regs, 0xFFu); /* DTL */
    assert(bus.fdc.phase == FDC_RESULT);
    assert(memcmp(bus.fdc.disk[0], payload, 1024) == 0); /* 実際に書かれた */
    while (bus.fdc.phase == FDC_RESULT)
        fdc_read_data(&bus.fdc); /* 結果を読み切ってFDC_IDLEへ戻す */

    /* 続けてSCAN EQUALを実行し、dma_cmp/scan_mismatchが正しく機能すること。 */
    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3); /* g_dma_payloadはpayloadのまま=一致 */
    assert(bus.fdc.res[2] == 0x08u);                  /* SH: WRITE DATAの内容と一致 */
    printf("test_t12_no_carryover_from_write: OK\n");
}

/* T13: SCANの直後にWRITE DATAを実行 → dma_cmpが確実に0へ戻り正しく書かれること。 */
static void test_t13_write_after_scan(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);

    uint8_t scan_target[1024] = {0};
    g_dma_payload = scan_target;
    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);
    while (bus.fdc.phase == FDC_RESULT)
        fdc_read_data(&bus.fdc);

    uint8_t payload[1024];
    for (int i = 0; i < 1024; i++)
        payload[i] = (uint8_t)(0xA5u ^ i);
    g_dma_payload = payload;

    fdc_write_data(&bus.fdc, ram, regs, 0x45u); /* WRITE DATA */
    fdc_write_data(&bus.fdc, ram, regs, 0x00u);
    fdc_write_data(&bus.fdc, ram, regs, 0x00u);
    fdc_write_data(&bus.fdc, ram, regs, 0x00u);
    fdc_write_data(&bus.fdc, ram, regs, 0x01u);
    fdc_write_data(&bus.fdc, ram, regs, 0x03u);
    fdc_write_data(&bus.fdc, ram, regs, 0x08u);
    fdc_write_data(&bus.fdc, ram, regs, 0x35u);
    fdc_write_data(&bus.fdc, ram, regs, 0xFFu);
    assert(memcmp(bus.fdc.disk[0], payload, 1024) == 0); /* 実際にディスクへ書かれた */
    printf("test_t13_write_after_scan: OK\n");
}

/* T14: 0x19(SCAN LOW OR EQUAL)は長さ9のみ登録・実行未実装。9バイト受理後
 * Invalid Commandへ落ち、ハングしないこと（design doc D1）。 */
static void test_t14_scan_low_unimplemented(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1024);

    fdc_write_data(&bus.fdc, ram, regs, 0x59u); /* MFM|SCAN LOW OR EQUAL(0x19) */
    for (int i = 0; i < 8; i++)
        fdc_write_data(&bus.fdc, ram, regs, 0x00u);

    assert(bus.fdc.res[0] == 0x80u); /* Invalid Command */
    assert(bus.fdc.res_len == 1);
    assert(bus.fdc.phase == FDC_RESULT);
    printf("test_t14_scan_low_unimplemented: OK\n");
}

/* T15: R=0（FDC直叩き、ROM側検証$FF92F6を経由しない）。design doc D11。
 * 修正前はdisk_offが負数キャストでラップし範囲チェックを素通りしていた。 */
static void test_t15_sector_zero(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 0, 3); /* R=0 */

    assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
    assert(bus.fdc.res[1] == 0x04u);
    printf("test_t15_sector_zero: OK\n");
}

/* T16: R=9（旧実装では次ヘッドR=1へエイリアスして通過した）。design doc D11。 */
static void test_t16_sector_nine(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 9, 3); /* R=9 */

    assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
    assert(bus.fdc.res[1] == 0x04u);
    printf("test_t16_sector_nine: OK\n");
}

/* T17/T18: N=32/N=255（左シフト未定義動作を経由せず弾かれること）。design doc D11。 */
static void test_t17_t18_n_out_of_range(void) {
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 32);
        assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
        assert(bus.fdc.res[1] == 0x04u);
    }
    {
        static x68k_bus_t bus = {0};
        bus.fdc.bus = &bus;
        uint8_t ram[16] = {0};
        uint8_t regs[32] = {0};
        insert_disk(&bus.fdc, 0, 1261568);
        send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 255);
        assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
        assert(bus.fdc.res[1] == 0x04u);
    }
    printf("test_t17_t18_n_out_of_range: OK\n");
}

/* T19: HD(cmd[1]由来)とH(cmd[3])が不一致。design doc D11。 */
static void test_t19_hd_head_mismatch(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    insert_disk(&bus.fdc, 0, 1261568);

    send_scan(&bus.fdc, ram, regs, 0, 1 /*hd*/, 0, 0 /*head*/, 1, 3); /* hd=1, head=0 */

    assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u));
    assert(bus.fdc.res[1] == 0x04u);
    printf("test_t19_hd_head_mismatch: OK\n");
}

/* T20: MAR不正フォールバック(dmac非武装+MAR範囲外) → 偽陽性のSHを返さず、
 * 専用の異常終了(ST0=AT, ST1=OR)へ遷移すること。design doc D12。 */
static void test_t20_mar_invalid_no_false_sh(void) {
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    uint8_t ram[16] = {0};
    static uint8_t regs[0x10000] = {0}; /* crtc_regs[0x400D-F]参照のためCRTC_SIZE相当を確保 */
    insert_disk(&bus.fdc, 0, 1261568);

    g_dmac_result = 0; /* 非武装 */
    regs[0x400Du] = 0xFFu;
    regs[0x400Eu] = 0xFFu;
    regs[0x400Fu] = 0xFFu; /* MAR = 0xFFFFFF、RAM_BUF_SIZEを超える */

    send_scan(&bus.fdc, ram, regs, 0, 0, 0, 0, 1, 3);

    assert(bus.fdc.res[0] == (uint8_t)(0x40u | 0u)); /* AT */
    assert(bus.fdc.res[1] == 0x10u);                 /* OR: Overrun */
    assert(bus.fdc.res[2] != 0x08u);                 /* SHではない */
    assert(bus.fdc.res[2] != 0x04u);                 /* SNでもない（比較未実施） */
    assert(bus.fdc.phase == FDC_RESULT);

    g_dmac_result = 1; /* 他のテストへ影響しないよう戻す */
    printf("test_t20_mar_invalid_no_false_sh: OK\n");
}

int main(void) {
    test_t1_command_length();
    test_t2_msr_during_exec();
    test_t2b_dma_dir_stays_zero();
    test_t3_match_sh_no_writeback();
    test_t4_mismatch_sn();
    test_t5_wildcard_ff();
    test_t6_wildcard_asymmetric();
    test_t7_last_byte_mismatch();
    test_t8_result_bytes();
    test_t9_not_ready();
    test_t10_out_of_range_disk_bytes();
    test_t11_sector_boundary();
    test_t12_no_carryover_from_write();
    test_t13_write_after_scan();
    test_t14_scan_low_unimplemented();
    test_t15_sector_zero();
    test_t16_sector_nine();
    test_t17_t18_n_out_of_range();
    test_t19_hd_head_mismatch();
    test_t20_mar_invalid_no_false_sh();

    printf("fdc_scan_test: ALL OK\n");
    return 0;
}
