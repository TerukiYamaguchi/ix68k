/* SPCプログラム転送(DREG連続書き込み)によるCOMMANDフェーズの単体テスト。
 * NetBSD/x68kのspcドライバはCDBをSet ACK/REQではなくSCMD.Transfer+DREG連続
 * 書き込みで送るため、DREG経路でもCDB長の確定が必要になる。 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* win_get_temp_path()（"/tmp/..."直書き置換用） */
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif

/* SPCレジスタ(addr = $E96020 + オフセット)。 */
#define SPC_REG_SCMD 0xE96025u
#define SPC_REG_INTS 0xE96029u
#define SPC_REG_PSNS 0xE9602Bu
#define SPC_REG_SSTS 0xE9602Du
#define SPC_REG_PCTL 0xE96031u
#define SPC_REG_DREG 0xE96035u
#define SPC_REG_TEMP 0xE96037u
#define SPC_REG_TCH  0xE96039u
#define SPC_REG_TCM  0xE9603Bu
#define SPC_REG_TCL  0xE9603Du

#define DMAC_CH1_CSR 0xE84040u
#define DMAC_CH1_OCR 0xE84045u
#define DMAC_CH1_SCR 0xE84046u
#define DMAC_CH1_CCR 0xE84047u
#define DMAC_CH1_MTC 0xE8404Au
#define DMAC_CH1_MAR 0xE8404Cu
#define DMAC_CH1_DAR 0xE84054u

#define SCMD_SELECT   0x20u
#define SCMD_TRANSFER 0x80u
#define SCMD_SET_AR   0xE0u
#define SCMD_RESET_AR 0xC0u

#define INTS_DC      0x20u
#define INTS_CC      0x10u
#define DMAC_CSR_COC 0x80u

#define SSTS_TC0  0x04u
#define SSTS_TRIP 0x10u
#define SSTS_DF   0x02u
#define SSTS_DE   0x01u

#define PSNS_PHASE_MASK 0x07u
#define PSNS_REQ        0x80u
#define PSNS_ACK        0x40u
#define PHASE_DATA_IN   0x01u
#define PHASE_STATUS    0x03u
#define PHASE_COMMAND   0x02u
#define PHASE_MSG_OUT   0x06u

#define PCTL_OUT_BFINT 0x82u /* Busfree INT Enable + フェーズコード(COMMAND) */
#define PCTL_IN_BFINT  0x81u /* Busfree INT Enable + フェーズコード(DATA IN) */

#define TEST_BLOCK_SIZE  512
#define TEST_BLOCK_COUNT 16
#define TEST_PATTERN_LBA 1

/* scsi_attach()が要求する最小のX68SCSI1ヘッダ(16B)を持つ一時イメージを作る。
 * blen=512, blocks=16。LBA=TEST_PATTERN_LBAには読み出し検証用の既知パターンを置く。 */
static void write_fake_scsi_image(const char *path) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    uint8_t hdr[16] = {0};
    memcpy(hdr, "X68SCSI1", 8);
    hdr[8] = 0x02; /* blen=512 (BE) */
    hdr[9] = 0x00;
    hdr[10] = 0x00; /* blocks=16 (BE) */
    hdr[11] = 0x00;
    hdr[12] = 0x00;
    hdr[13] = 0x10;
    hdr[0x0E] = 0x00;
    hdr[0x0F] = 0x00; /* EJ=0 (固定ディスク扱い) */
    fwrite(hdr, 1, sizeof(hdr), fp);
    uint8_t zero[TEST_BLOCK_SIZE] = {0};
    for (int i = 0; i < TEST_BLOCK_COUNT; i++)
        fwrite(zero, 1, sizeof(zero), fp);
    /* イメージ上のLBA nはファイル先頭からn*blenの位置(scsi_unit_read準拠)。 */
    uint8_t pattern[TEST_BLOCK_SIZE];
    for (int i = 0; i < TEST_BLOCK_SIZE; i++)
        pattern[i] = (uint8_t)(i ^ 0x5A);
    fseek(fp, (long)TEST_PATTERN_LBA * TEST_BLOCK_SIZE, SEEK_SET);
    fwrite(pattern, 1, sizeof(pattern), fp);
    fclose(fp);
}

#ifdef _WIN32
static void temp_path(char *buf, size_t n, const char *name) {
    win_get_temp_path(buf, (int)n, name);
}
#else
static void temp_path(char *buf, size_t n, const char *name) {
    snprintf(buf, n, "/tmp/%s", name);
}
#endif

static void set_tc(x68k_bus_t *b, uint32_t tc) {
    x68k_bus_write8(b, SPC_REG_TCH, (uint8_t)(tc >> 16));
    x68k_bus_write8(b, SPC_REG_TCM, (uint8_t)(tc >> 8));
    x68k_bus_write8(b, SPC_REG_TCL, (uint8_t)tc);
}

/* ターゲットを選択し、選択完了(INTS.CC)をクリアしてCOMMANDフェーズ待ちにする。 */
static void select_target(x68k_bus_t *b, int id) {
    x68k_bus_write8(b, SPC_REG_TEMP, (uint8_t)(1u << id)); /* TEMP: 対象ID */
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_SELECT);
    uint8_t ints = x68k_bus_read8(b, SPC_REG_INTS);
    assert((ints & INTS_CC) != 0); /* Selectの完了通知 */
    x68k_bus_write8(b, SPC_REG_INTS, ints);
    assert(x68k_bus_read8(b, SPC_REG_INTS) == 0);
}

/* NetBSD spc_dataout_pio()と同じ手順でCDBを送る:
 * TCにCDB長を設定 → PCTL/SCMD.Transfer → DREGへ連続書き込み。 */
static void send_cdb_via_dreg(x68k_bus_t *b, const uint8_t *cdb, int len) {
    set_tc(b, (uint32_t)len);
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_OUT_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    for (int i = 0; i < len; i++)
        x68k_bus_write8(b, SPC_REG_DREG, cdb[i]);
}

/* 従来のCPU 1バイト転送(Set ACK/REQ + Reset ACK/REQ)でCDBを送る。 */
static void send_cdb_via_set_ar(x68k_bus_t *b, const uint8_t *cdb, int len) {
    for (int i = 0; i < len; i++) {
        x68k_bus_write8(b, SPC_REG_TEMP, cdb[i]);
        x68k_bus_write8(b, SPC_REG_SCMD, SCMD_SET_AR);
        x68k_bus_write8(b, SPC_REG_SCMD, SCMD_RESET_AR);
    }
}

static void read_data_in(x68k_bus_t *b, uint8_t *out, int n) {
    set_tc(b, (uint32_t)n);
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_IN_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    for (int i = 0; i < n; i++)
        out[i] = x68k_bus_read8(b, SPC_REG_DREG);
}

static uint16_t read_dmac_ch1_mtc(x68k_bus_t *b) {
    return (uint16_t)((uint16_t)x68k_bus_read8(b, DMAC_CH1_MTC) << 8) |
           x68k_bus_read8(b, DMAC_CH1_MTC + 1);
}

/* ch1の設定はOCR.DIR=0(MAR→DREG)、SCR.MAC=++、DAR=DREGで固定する。 */
static void dmac_ch1_to_spc(x68k_bus_t *b, uint32_t mar, uint16_t count) {
    x68k_bus_write8(b, DMAC_CH1_OCR, 0x31u);
    x68k_bus_write8(b, DMAC_CH1_SCR, 0x04u);
    x68k_bus_write8(b, DMAC_CH1_MTC, (uint8_t)(count >> 8));
    x68k_bus_write8(b, DMAC_CH1_MTC + 1, (uint8_t)count);
    x68k_bus_write8(b, DMAC_CH1_MAR + 1, (uint8_t)(mar >> 16));
    x68k_bus_write8(b, DMAC_CH1_MAR + 2, (uint8_t)(mar >> 8));
    x68k_bus_write8(b, DMAC_CH1_MAR + 3, (uint8_t)mar);
    x68k_bus_write8(b, DMAC_CH1_DAR, 0);
    x68k_bus_write8(b, DMAC_CH1_DAR + 1, 0xE9u);
    x68k_bus_write8(b, DMAC_CH1_DAR + 2, 0x60u);
    x68k_bus_write8(b, DMAC_CH1_DAR + 3, 0x35u);
    x68k_bus_write8(b, DMAC_CH1_CCR, 0x80u);
}

static void dmac_ch1_from_spc(x68k_bus_t *b, uint32_t mar, uint16_t count) {
    x68k_bus_write8(b, DMAC_CH1_OCR, 0xB1u);
    x68k_bus_write8(b, DMAC_CH1_SCR, 0x04u);
    x68k_bus_write8(b, DMAC_CH1_MTC, (uint8_t)(count >> 8));
    x68k_bus_write8(b, DMAC_CH1_MTC + 1, (uint8_t)count);
    x68k_bus_write8(b, DMAC_CH1_MAR + 1, (uint8_t)(mar >> 16));
    x68k_bus_write8(b, DMAC_CH1_MAR + 2, (uint8_t)(mar >> 8));
    x68k_bus_write8(b, DMAC_CH1_MAR + 3, (uint8_t)mar);
    x68k_bus_write8(b, DMAC_CH1_DAR, 0);
    x68k_bus_write8(b, DMAC_CH1_DAR + 1, 0xE9u);
    x68k_bus_write8(b, DMAC_CH1_DAR + 2, 0x60u);
    x68k_bus_write8(b, DMAC_CH1_DAR + 3, 0x35u);
    x68k_bus_write8(b, DMAC_CH1_CCR, 0x80u);
}

static x68k_bus_t *make_bus_with_disk(const char *path) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    write_fake_scsi_image(path);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    return b;
}

/* 6バイトCDB(Test Unit Ready)をDREG連続書き込みで送り、全6バイトが受領されて
 * コマンド完了(INTS.CC)とSTATUSフェーズ遷移が起きること。 */
static void test_dreg_6byte_cdb_completes(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dreg_cdb_6.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);

    select_target(b, 0);
    const uint8_t cdb[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; /* Test Unit Ready */
    send_cdb_via_dreg(b, cdb, (int)sizeof(cdb));

    assert((x68k_bus_read8(b, SPC_REG_SSTS) & SSTS_TC0) != 0); /* 6バイト分TCが減った */
    assert((x68k_bus_read8(b, SPC_REG_INTS) & INTS_CC) != 0);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);
    assert(x68k_bus_read8(b, SPC_REG_TEMP) == 0x00); /* GOOD */

    x68k_bus_destroy(b);
    unlink(path);
    printf("test_dreg_6byte_cdb_completes: OK\n");
}

/* 10バイトCDB(Read(10))をDREG連続書き込みで送り、CDB[2..5]のLBAと
 * CDB[7..8]の転送ブロック数まで正しく格納されること（＝10バイト全部が届くこと）。 */
static void test_dreg_10byte_cdb_completes(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dreg_cdb_10.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);

    select_target(b, 0);
    /* Read(10): CDB[2..5]=LBA、CDB[7..8]=転送ブロック数。 */
    const uint8_t cdb[10] = {0x28, 0x00, 0x00, 0x00, 0x00, TEST_PATTERN_LBA,
                             0x00, 0x00, 0x01, 0x00};
    send_cdb_via_dreg(b, cdb, (int)sizeof(cdb));

    assert((x68k_bus_read8(b, SPC_REG_INTS) & INTS_CC) != 0);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_DATA_IN);

    uint8_t data[TEST_BLOCK_SIZE];
    read_data_in(b, data, (int)sizeof(data));
    for (int i = 0; i < TEST_BLOCK_SIZE; i++)
        assert(data[i] == (uint8_t)(i ^ 0x5A));

    x68k_bus_destroy(b);
    unlink(path);
    printf("test_dreg_10byte_cdb_completes: OK\n");
}

/* 既存のSet ACK/REQ経路(内蔵SCSI ROMが使う)に退行がないこと。 */
static void test_set_ar_path_still_works(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dreg_cdb_setar.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);

    select_target(b, 0);
    /* Read(10)。DREG経路(上のテスト)と同じCDBを従来経路で送る。 */
    const uint8_t cdb[10] = {0x28, 0x00, 0x00, 0x00, 0x00, TEST_PATTERN_LBA,
                             0x00, 0x00, 0x01, 0x00};
    send_cdb_via_set_ar(b, cdb, (int)sizeof(cdb));

    assert((x68k_bus_read8(b, SPC_REG_INTS) & INTS_CC) != 0);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_DATA_IN);

    uint8_t data[TEST_BLOCK_SIZE];
    read_data_in(b, data, (int)sizeof(data));
    for (int i = 0; i < TEST_BLOCK_SIZE; i++)
        assert(data[i] == (uint8_t)(i ^ 0x5A));

    x68k_bus_destroy(b);
    unlink(path);
    printf("test_set_ar_path_still_works: OK\n");
}

/* REQUEST SENSEのAllocation Length=32に対し、SCSI-2固定長の応答は18バイト。
 * 短転送後の余剰DREG読みがSTATUS/MSG INを消費せず、TCとFIFO状態を保つこと。 */
static void test_short_data_in_preserves_status_after_extra_dreg_reads(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dreg_short_read.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);

    /* 未実装コマンドでILLEGAL REQUESTのセンスを用意する。 */
    select_target(b, 0);
    const uint8_t invalid_cdb[6] = {0x02, 0, 0, 0, 0, 0};
    send_cdb_via_set_ar(b, invalid_cdb, (int)sizeof(invalid_cdb));
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);

    select_target(b, 0);
    const uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 32, 0};
    send_cdb_via_dreg(b, sense_cdb, (int)sizeof(sense_cdb));
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_DATA_IN);

    set_tc(b, 32);
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_IN_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    for (int i = 0; i < 18; i++)
        (void)x68k_bus_read8(b, SPC_REG_DREG);

    uint8_t ssts = x68k_bus_read8(b, SPC_REG_SSTS);
    assert((ssts & SSTS_TRIP) == 0);
    assert((ssts & SSTS_DF) == 0);
    assert((ssts & SSTS_DE) != 0);
    assert(x68k_bus_read8(b, SPC_REG_TCL) == 14);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);

    for (int i = 0; i < 14; i++)
        (void)x68k_bus_read8(b, SPC_REG_DREG);
    assert(x68k_bus_read8(b, SPC_REG_TCL) == 14);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);

    x68k_bus_destroy(b);
    unlink(path);
    printf("test_short_data_in_preserves_status_after_extra_dreg_reads: OK\n");
}

/* TC=0のTransferでは、DREG書き込みがCOMMANDの1バイト目を受領してはならない。 */
static void test_dreg_write_requires_nonzero_tc(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dreg_zero_tc.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);

    select_target(b, 0);
    set_tc(b, 0);
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_OUT_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    x68k_bus_write8(b, SPC_REG_DREG, 0x00);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) != PHASE_STATUS);
    assert((x68k_bus_read8(b, SPC_REG_SSTS) & SSTS_TC0) != 0);

    x68k_bus_destroy(b);
    unlink(path);
    printf("test_dreg_write_requires_nonzero_tc: OK\n");
}

/* DMACはCOMMAND先頭バイト後にもSPCゲートを再評価し、CDB全体を送る。 */
static void test_dmac_command_transfers_full_cdb(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dmac_cdb.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);
    const uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};
    const uint32_t mar = 0x001000u;
    for (int i = 0; i < (int)sizeof(cdb); i++)
        x68k_bus_write8(b, mar + (uint32_t)i, cdb[i]);

    select_target(b, 0);
    set_tc(b, sizeof(cdb));
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_OUT_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    dmac_ch1_to_spc(b, mar, sizeof(cdb));

    assert(read_dmac_ch1_mtc(b) == 0);
    assert((x68k_bus_read8(b, DMAC_CH1_CSR) & DMAC_CSR_COC) != 0);
    assert((x68k_bus_read8(b, SPC_REG_INTS) & INTS_CC) != 0);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);
    x68k_bus_destroy(b);
    unlink(path);
    printf("test_dmac_command_transfers_full_cdb: OK\n");
}

/* DMAC経由でも10バイトCDBを先頭バイトだけで止めず、DATA INへ遷移する。 */
static void test_dmac_10byte_cdb_transfers_full(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dmac_cdb10.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);
    const uint8_t cdb[10] = {0x28, 0, 0, 0, 0, TEST_PATTERN_LBA, 0, 0, 1, 0};
    const uint32_t mar = 0x001100u;
    for (int i = 0; i < (int)sizeof(cdb); i++)
        x68k_bus_write8(b, mar + (uint32_t)i, cdb[i]);

    select_target(b, 0);
    set_tc(b, sizeof(cdb));
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_OUT_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    dmac_ch1_to_spc(b, mar, sizeof(cdb));

    assert(read_dmac_ch1_mtc(b) == 0);
    assert((x68k_bus_read8(b, DMAC_CH1_CSR) & DMAC_CSR_COC) != 0);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_DATA_IN);
    x68k_bus_destroy(b);
    unlink(path);
    printf("test_dmac_10byte_cdb_transfers_full: OK\n");
}

/* DMA DATA IN短転送では、実転送分だけMTCを減らしCOCを立てない。 */
static void test_dmac_short_data_in_preserves_mtc(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dmac_short_read.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);
    const uint8_t invalid_cdb[6] = {0x02, 0, 0, 0, 0, 0};
    const uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 32, 0};

    select_target(b, 0);
    send_cdb_via_set_ar(b, invalid_cdb, sizeof(invalid_cdb));
    select_target(b, 0);
    send_cdb_via_dreg(b, sense_cdb, sizeof(sense_cdb));
    set_tc(b, 32);
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_IN_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    dmac_ch1_from_spc(b, 0x001100u, 32);

    assert(read_dmac_ch1_mtc(b) == 14);
    assert((x68k_bus_read8(b, DMAC_CH1_CSR) & DMAC_CSR_COC) == 0);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);
    x68k_bus_destroy(b);
    unlink(path);
    printf("test_dmac_short_data_in_preserves_mtc: OK\n");
}

/* DMAC ch1のDATA OUTは実データ、MTC/COC、STATUS遷移をすべて進める。 */
static void test_dmac_data_out_writes_full_block(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_dmac_write.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);
    const uint8_t cdb[6] = {0x0A, 0, 0, 1, 1, 0}; /* Write(6), LBA 1 */
    const uint32_t cdb_mar = 0x001200u;
    const uint32_t data_mar = 0x001300u;
    uint8_t data[TEST_BLOCK_SIZE];
    for (int i = 0; i < (int)sizeof(cdb); i++)
        x68k_bus_write8(b, cdb_mar + (uint32_t)i, cdb[i]);
    for (int i = 0; i < TEST_BLOCK_SIZE; i++) {
        data[i] = (uint8_t)(i ^ 0xC3);
        x68k_bus_write8(b, data_mar + (uint32_t)i, data[i]);
    }

    select_target(b, 0);
    set_tc(b, sizeof(cdb));
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_OUT_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    dmac_ch1_to_spc(b, cdb_mar, sizeof(cdb));
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == 0); /* DATA OUT */
    x68k_bus_write8(b, DMAC_CH1_CSR, DMAC_CSR_COC); /* 次のDMAに備えCOCをクリア */

    set_tc(b, TEST_BLOCK_SIZE);
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_OUT_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    dmac_ch1_to_spc(b, data_mar, TEST_BLOCK_SIZE);
    assert(read_dmac_ch1_mtc(b) == 0);
    assert((x68k_bus_read8(b, DMAC_CH1_CSR) & DMAC_CSR_COC) != 0);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);

    x68k_bus_destroy(b);
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    assert(fseek(fp, TEST_BLOCK_SIZE, SEEK_SET) == 0);
    uint8_t stored[TEST_BLOCK_SIZE];
    assert(fread(stored, 1, sizeof(stored), fp) == sizeof(stored));
    fclose(fp);
    assert(memcmp(stored, data, sizeof(data)) == 0);
    unlink(path);
    printf("test_dmac_data_out_writes_full_block: OK\n");
}

static void test_message_out_then_command(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_message_out.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);
    const uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};

    x68k_bus_write8(b, SPC_REG_SCMD, 0x60u); /* Set ATN */
    select_target(b, 0);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_MSG_OUT);
    x68k_bus_write8(b, SPC_REG_TEMP, 0x80u); /* IDENTIFY, LUN 0 */
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_SET_AR);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_RESET_AR);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_COMMAND);
    send_cdb_via_set_ar(b, cdb, sizeof(cdb));
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);
    x68k_bus_destroy(b);
    unlink(path);
    printf("test_message_out_then_command: OK\n");
}

/* TCがDATA OUT全長より短い場合、未転送領域をxfer_bufの残留値で上書きしない。 */
static void test_short_data_out_writes_only_received_bytes(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_short_write.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);
    const uint8_t cdb[6] = {0x0A, 0, 0, 1, 1, 0}; /* Write(6), LBA 1, 1 block */
    uint8_t received[256];
    for (int i = 0; i < (int)sizeof(received); i++)
        received[i] = (uint8_t)(i ^ 0xA5);

    select_target(b, 0);
    send_cdb_via_dreg(b, cdb, sizeof(cdb));
    set_tc(b, sizeof(received));
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_OUT_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    for (int i = 0; i < (int)sizeof(received); i++)
        x68k_bus_write8(b, SPC_REG_DREG, received[i]);
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);

    x68k_bus_destroy(b);
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    assert(fseek(fp, TEST_BLOCK_SIZE, SEEK_SET) == 0);
    uint8_t stored[TEST_BLOCK_SIZE];
    assert(fread(stored, 1, sizeof(stored), fp) == sizeof(stored));
    fclose(fp);
    assert(memcmp(stored, received, sizeof(received)) == 0);
    for (int i = (int)sizeof(received); i < TEST_BLOCK_SIZE; i++)
        assert(stored[i] == (uint8_t)(i ^ 0x5A)); /* 元のLBA 1パターンを保持 */
    unlink(path);
    printf("test_short_data_out_writes_only_received_bytes: OK\n");
}

/* STATUSとMESSAGE INをDREGで1バイトずつ受け、BUS FREE/DCまで到達する。 */
static void test_status_message_in_and_bus_free_via_dreg(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_status_msg_busfree.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);
    const uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};

    select_target(b, 0);
    send_cdb_via_dreg(b, cdb, sizeof(cdb));
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);

    set_tc(b, 1);
    x68k_bus_write8(b, SPC_REG_PCTL, PCTL_IN_BFINT);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    assert(x68k_bus_read8(b, SPC_REG_DREG) == 0x00);                      /* GOOD */
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == 0x07u); /* MSG IN */

    set_tc(b, 1);
    x68k_bus_write8(b, SPC_REG_SCMD, SCMD_TRANSFER);
    assert(x68k_bus_read8(b, SPC_REG_DREG) == 0x00); /* COMMAND COMPLETE */
    assert(x68k_bus_read8(b, SPC_REG_PSNS) == 0);
    assert((x68k_bus_read8(b, SPC_REG_INTS) & INTS_DC) != 0);

    x68k_bus_destroy(b);
    unlink(path);
    printf("test_status_message_in_and_bus_free_via_dreg: OK\n");
}

/* Set/Reset ACK/REQによるDATA OUTはTCを消費せず、REQ/ACKを手動でトグルする。 */
static void test_data_out_via_set_reset_ack_req(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_manual_data_out.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);
    const uint8_t cdb[6] = {0x0A, 0, 0, 1, 1, 0}; /* Write(6), LBA 1 */
    uint8_t data[TEST_BLOCK_SIZE];
    for (int i = 0; i < TEST_BLOCK_SIZE; i++)
        data[i] = (uint8_t)(i ^ 0x3C);

    select_target(b, 0);
    send_cdb_via_set_ar(b, cdb, sizeof(cdb));
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == 0); /* DATA OUT */
    assert((x68k_bus_read8(b, SPC_REG_SSTS) & SSTS_TC0) != 0);
    for (int i = 0; i < TEST_BLOCK_SIZE; i++) {
        x68k_bus_write8(b, SPC_REG_TEMP, data[i]);
        x68k_bus_write8(b, SPC_REG_SCMD, SCMD_SET_AR);
        uint8_t psns = x68k_bus_read8(b, SPC_REG_PSNS);
        assert((psns & (PSNS_REQ | PSNS_ACK)) == PSNS_ACK);
        x68k_bus_write8(b, SPC_REG_SCMD, SCMD_RESET_AR);
        if (i + 1 < TEST_BLOCK_SIZE) {
            psns = x68k_bus_read8(b, SPC_REG_PSNS);
            assert((psns & (PSNS_REQ | PSNS_ACK)) == PSNS_REQ);
        }
    }
    assert((x68k_bus_read8(b, SPC_REG_PSNS) & PSNS_PHASE_MASK) == PHASE_STATUS);
    assert((x68k_bus_read8(b, SPC_REG_SSTS) & SSTS_TC0) != 0);

    x68k_bus_destroy(b);
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    assert(fseek(fp, TEST_BLOCK_SIZE, SEEK_SET) == 0);
    uint8_t stored[TEST_BLOCK_SIZE];
    assert(fread(stored, 1, sizeof(stored), fp) == sizeof(stored));
    fclose(fp);
    assert(memcmp(stored, data, sizeof(data)) == 0);
    unlink(path);
    printf("test_data_out_via_set_reset_ack_req: OK\n");
}

/* TCの各バイト書き込みはTransfer開始前でもSSTS.TC0へ即時反映される。 */
static void test_tc_write_updates_ssts_immediately(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_tc_ssts.HDS");
    x68k_bus_t *b = make_bus_with_disk(path);

    select_target(b, 0);
    assert((x68k_bus_read8(b, SPC_REG_SSTS) & SSTS_TC0) != 0);
    x68k_bus_write8(b, SPC_REG_TCL, 1);
    assert((x68k_bus_read8(b, SPC_REG_SSTS) & SSTS_TC0) == 0);
    x68k_bus_write8(b, SPC_REG_TCL, 0);
    assert((x68k_bus_read8(b, SPC_REG_SSTS) & SSTS_TC0) != 0);

    x68k_bus_destroy(b);
    unlink(path);
    printf("test_tc_write_updates_ssts_immediately: OK\n");
}

int main(void) {
    test_dreg_6byte_cdb_completes();
    test_dreg_10byte_cdb_completes();
    test_set_ar_path_still_works();
    test_short_data_in_preserves_status_after_extra_dreg_reads();
    test_dreg_write_requires_nonzero_tc();
    test_dmac_command_transfers_full_cdb();
    test_dmac_10byte_cdb_transfers_full();
    test_dmac_short_data_in_preserves_mtc();
    test_dmac_data_out_writes_full_block();
    test_message_out_then_command();
    test_short_data_out_writes_only_received_bytes();
    test_status_message_in_and_bus_free_via_dreg();
    test_data_out_via_set_reset_ack_req();
    test_tc_write_updates_ssts_immediately();
    printf("scsi_dreg_cdb_test: all tests passed\n");
    return 0;
}
