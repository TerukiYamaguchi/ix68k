/* SCSI CD-ROM(CloneCD)メディア層の単体テスト(P1)。
 * docs/ix68k-internals/design/scsi_design.md §4/§9準拠。ローカルの実イメージ(245MB)には
 * 依存せず、数セクタの最小CloneCDフィクスチャ(.ccd+.img)を都度生成して検証する。
 * 実イメージによる確認は統合テスト(P2以降、MCP/実機検証)として別枠。 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* win_get_temp_template()/mkdtemp()（プロセス一意な
                                   * 一時ディレクトリ作成用） */
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif

#define FIX_RAW_SECTOR  2352
#define FIX_USER_OFFSET 16
#define FIX_USER_SIZE   2048

/* プロセス一意な一時ディレクトリを初回呼び出し時にのみ作成し、以降はキャッシュを返す
 * （tests/fdc_eject_test.cと同じmkdtemp()/win_get_temp_template()パターン）。
 * 固定"/tmp/<ファイル名>"を複数プロセスが同時に読み書きして競合・abortする問題への対策。
 * テストバイナリはシングルスレッドのため排他制御は不要。 */
static const char *fixture_dir(void) {
    static char dir[300];
    static int initialized = 0;
    if (!initialized) {
#ifdef _WIN32
        win_get_temp_template(dir, sizeof(dir), "scsi_cdrom_test.");
#else
        snprintf(dir, sizeof(dir), "/tmp/scsi_cdrom_test.XXXXXX");
#endif
        assert(mkdtemp(dir) != NULL);
        initialized = 1;
    }
    return dir;
}

static const char *fixture_path(char *buf, size_t bufsz, const char *filename) {
    snprintf(buf, bufsz, "%s/%s", fixture_dir(), filename);
    return buf;
}

/* n_sectors分のMode1セクタを持つ.imgを書く。各セクタのユーザデータ
 * (offset16から2048バイト)には`(セクタ番号*31 + オフセット)`を書き込み、
 * セクタ位置に応じて内容が変わるパターンにする（誤ったLBAやオフセットで
 * 読んだ場合に検出できるように）。同期/ヘッダ/EDC-ECC部分はゼロ埋め
 * （scsi_unit_read()はユーザデータ部分しか見ないため無関係）。 */
static void write_fixture_img(const char *path, uint32_t n_sectors) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    uint8_t sector[FIX_RAW_SECTOR];
    for (uint32_t i = 0; i < n_sectors; i++) {
        memset(sector, 0, sizeof(sector));
        for (int j = 0; j < FIX_USER_SIZE; j++)
            sector[FIX_USER_OFFSET + j] = (uint8_t)(i * 31 + j);
        size_t written = fwrite(sector, 1, sizeof(sector), fp);
        assert(written == sizeof(sector));
    }
    fclose(fp);
}

/* 最小の.ccdを書く。単一データトラック(トラック1、LBA0開始、Mode1)、
 * リードアウトLBA=leadout_lba。実物(disk/配下の対象イメージ)と同じ
 * Point/ADR/Control構成(ADR=0x01, Control=0x06)。 */
static void write_fixture_ccd(const char *path, uint32_t leadout_lba) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fprintf(fp,
            "[CloneCD]\r\n"
            "Version=3\r\n"
            "[Disc]\r\n"
            "TocEntries=4\r\n"
            "Sessions=1\r\n"
            "DataTracksScrambled=0\r\n"
            "CDTextLength=0\r\n"
            "[Entry 0]\r\n"
            "Point=0xa0\r\n"
            "ADR=0x01\r\n"
            "Control=0x06\r\n"
            "PMin=1\r\n"
            "PSec=0\r\n"
            "PFrame=0\r\n"
            "[Entry 1]\r\n"
            "Point=0xa1\r\n"
            "ADR=0x01\r\n"
            "Control=0x06\r\n"
            "PMin=1\r\n"
            "PSec=0\r\n"
            "PFrame=0\r\n"
            "[Entry 2]\r\n"
            "Point=0xa2\r\n"
            "ADR=0x01\r\n"
            "Control=0x06\r\n"
            "PLBA=%u\r\n"
            "[Entry 3]\r\n"
            "Point=0x01\r\n"
            "ADR=0x01\r\n"
            "Control=0x06\r\n"
            "PLBA=0\r\n"
            "[TRACK 1]\r\n"
            "MODE=1\r\n"
            "FLAGS= DCP\r\n"
            "INDEX 1=0\r\n",
            leadout_lba);
    fclose(fp);
}

static void write_fixture_ccd_scrambled(const char *path, uint32_t leadout_lba) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fprintf(fp,
            "[CloneCD]\r\n"
            "Version=3\r\n"
            "[Disc]\r\n"
            "TocEntries=4\r\n"
            "Sessions=1\r\n"
            "DataTracksScrambled=1\r\n"
            "CDTextLength=0\r\n"
            "[Entry 0]\r\n"
            "Point=0xa0\r\nADR=0x01\r\nControl=0x06\r\nPMin=1\r\nPSec=0\r\nPFrame=0\r\n"
            "[Entry 1]\r\n"
            "Point=0xa1\r\nADR=0x01\r\nControl=0x06\r\nPMin=1\r\nPSec=0\r\nPFrame=0\r\n"
            "[Entry 2]\r\n"
            "Point=0xa2\r\nADR=0x01\r\nControl=0x06\r\nPLBA=%u\r\n"
            "[Entry 3]\r\n"
            "Point=0x01\r\nADR=0x01\r\nControl=0x06\r\nPLBA=0\r\n"
            "[TRACK 1]\r\n"
            "MODE=1\r\n"
            "FLAGS= DCP\r\n"
            "INDEX 1=0\r\n",
            leadout_lba);
    fclose(fp);
}

static void write_fixture_ccd_mode2(const char *path, uint32_t leadout_lba) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fprintf(fp,
            "[CloneCD]\r\n"
            "Version=3\r\n"
            "[Disc]\r\n"
            "TocEntries=4\r\n"
            "Sessions=1\r\n"
            "DataTracksScrambled=0\r\n"
            "CDTextLength=0\r\n"
            "[Entry 0]\r\n"
            "Point=0xa0\r\nADR=0x01\r\nControl=0x06\r\nPMin=1\r\nPSec=0\r\nPFrame=0\r\n"
            "[Entry 1]\r\n"
            "Point=0xa1\r\nADR=0x01\r\nControl=0x06\r\nPMin=1\r\nPSec=0\r\nPFrame=0\r\n"
            "[Entry 2]\r\n"
            "Point=0xa2\r\nADR=0x01\r\nControl=0x06\r\nPLBA=%u\r\n"
            "[Entry 3]\r\n"
            "Point=0x01\r\nADR=0x01\r\nControl=0x06\r\nPLBA=0\r\n"
            "[TRACK 1]\r\n"
            "MODE=2\r\n" /* Mode2 Form1等は非スコープ */
            "FLAGS= DCP\r\n"
            "INDEX 1=0\r\n",
            leadout_lba);
    fclose(fp);
}

/* scsi_attach()が要求する最小のX68SCSI1ヘッダ(16B)を持つ一時イメージ
 * ファイルを作る（tests/scsi_status_test.cのwrite_fake_scsi_image()と同じ）。 */
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
    uint8_t zero[512] = {0};
    for (int i = 0; i < 16; i++)
        fwrite(zero, 1, sizeof(zero), fp);
    fclose(fp);
}

/* SELECTフェーズを駆動する（tests/scsi_status_test.cと同じ）。 */
static void select_target(x68k_bus_t *b, int id) {
    x68k_bus_write8(b, 0xE96037u, (uint8_t)(1u << id)); /* TEMP: 対象ID */
    x68k_bus_write8(b, 0xE96025u, 0x20u);               /* SCMD: Select */
}

/* COMMANDフェーズを6バイトCDBで駆動し、STATUSフェーズの状態バイトを読む
 * （tests/scsi_status_test.cのdrive_command_and_get_status()と同じ）。 */
static uint8_t drive_command_and_get_status(x68k_bus_t *b, const uint8_t cdb[6]) {
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]); /* TEMP: 送信バイト */
        x68k_bus_write8(b, 0xE96025u, 0xE0u);  /* SCMD: Set ACK/REQ */
        x68k_bus_write8(b, 0xE96025u, 0xC0u);  /* SCMD: Reset ACK/REQ */
    }
    return x68k_bus_read8(b, 0xE96037u); /* TEMP: STATUSフェーズの状態バイト */
}

/* リムーバブルメディア挿入直後の最初のコマンドはUNIT ATTENTIONを返すため、
 * Test Unit Readyで一度消費してから狙ったコマンドを送りたいテストで使う。 */
static void clear_unit_attention(x68k_bus_t *b, int id) {
    select_target(b, id);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    (void)drive_command_and_get_status(b, tur_cdb);
}

/* COMMANDフェーズを6バイトCDBで駆動し、DATA_INフェーズからnバイト読み出す
 * （tests/scsi_status_test.cのdrive_command_and_read_data()をn>255対応に
 * 拡張: TCH/TCMも書く。CD-ROMの1ブロック=2048バイトは8bitのTCLに収まらない）。 */
static void drive_command_and_read_data(x68k_bus_t *b, const uint8_t cdb[6], int n,
                                        uint8_t *out) {
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]); /* TEMP: 送信バイト */
        x68k_bus_write8(b, 0xE96025u, 0xE0u);  /* SCMD: Set ACK/REQ */
        x68k_bus_write8(b, 0xE96025u, 0xC0u);  /* SCMD: Reset ACK/REQ */
    }
    x68k_bus_write8(b, 0xE96039u, (uint8_t)(n >> 16)); /* TCH */
    x68k_bus_write8(b, 0xE9603Bu, (uint8_t)(n >> 8));  /* TCM */
    x68k_bus_write8(b, 0xE9603Du, (uint8_t)n);         /* TCL */
    x68k_bus_write8(b, 0xE96031u, 0x81u);              /* PCTL: DATA IN + Busfree INT */
    x68k_bus_write8(b, 0xE96025u, 0x80u);              /* SCMD: Transfer */
    for (int i = 0; i < n; i++)
        out[i] = x68k_bus_read8(b, 0xE96035u); /* DREG */
}

/* CCD解析: Point 0xA0/0xA1/0xA2/0x01の読み取りとADR<<4|Controlの計算を確認する。 */
static void test_cdrom_attach_parses_ccd(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_a.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_a.img");
    const uint32_t leadout = 4;
    write_fixture_ccd(ccd_path, leadout);
    write_fixture_img(img_path, leadout);

    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);

    scsi_unit_t *u = &b->spc_in.units[0];
    assert(u->kind == SCSI_UNIT_CDROM);
    assert(u->bytes_per_record == SCSI_CD_BLOCK_SIZE);
    assert(u->disk_end_record == leadout);
    assert(u->removable == 1);
    assert(u->write_protected == 1);
    assert(u->cd_first_track == 1);
    assert(u->cd_last_track == 1);
    assert(u->cd_track_count == 1);
    assert(u->cd_tracks[0].track_no == 1);
    assert(u->cd_tracks[0].start_lba == 0);
    assert(u->cd_tracks[0].adr_control == 0x16); /* (ADR<<4)|Control = (1<<4)|6 */

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_attach_parses_ccd: OK\n");
}

/* Read(6)経由で2352Bセクタからの2048Bユーザデータ抽出(offset16スキップ)を確認する。 */
static void test_cdrom_read_extracts_user_data(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_read.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_read.img");
    const uint32_t leadout = 4;
    write_fixture_ccd(ccd_path, leadout);
    write_fixture_img(img_path, leadout);
    assert(x68k_bus_scsi_insert(b, 1, ccd_path) == 0);
    clear_unit_attention(b, 1);

    select_target(b, 1);
    /* Read(6): LBA=2, n=1(単一ブロック) */
    uint8_t cdb[6] = {0x08, 0, 0, 2, 1, 0};
    uint8_t data[FIX_USER_SIZE];
    drive_command_and_read_data(b, cdb, FIX_USER_SIZE, data);

    uint8_t expected[FIX_USER_SIZE];
    for (int j = 0; j < FIX_USER_SIZE; j++)
        expected[j] = (uint8_t)(2 * 31 + j);
    assert(memcmp(data, expected, FIX_USER_SIZE) == 0);

    x68k_bus_scsi_eject(b, 1);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_read_extracts_user_data: OK\n");
}

/* DataTracksScrambled!=0のイメージは拒否する(非スコープ、scsi_design.md §9)。 */
static void test_cdrom_rejects_scrambled(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_scrambled.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_scrambled.img");
    write_fixture_ccd_scrambled(ccd_path, 4);
    write_fixture_img(img_path, 4);

    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == -1);

    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_rejects_scrambled: OK\n");
}

/* MODE=1以外(Mode2 Form1等)のデータトラックは拒否する(非スコープ)。 */
static void test_cdrom_rejects_non_mode1(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_mode2.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_mode2.img");
    write_fixture_ccd_mode2(ccd_path, 4);
    write_fixture_img(img_path, 4);

    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == -1);

    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_rejects_non_mode1: OK\n");
}

/* .ccd記載のリードアウトLBAより.imgの実サイズが小さい場合、実測セクタ数を
 * 採用する（切り詰めイメージへの耐性）。 */
static void test_cdrom_truncated_img_uses_actual_size(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_trunc.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_trunc.img");
    write_fixture_ccd(ccd_path, 8); /* .ccdは8セクタと主張 */
    write_fixture_img(img_path, 4); /* 実体は4セクタしかない */

    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);
    assert(b->spc_in.units[0].disk_end_record == 4);

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_truncated_img_uses_actual_size: OK\n");
}

/* 既存HDD形式(X68SCSI1)は回帰なく引き続き認識される。 */
static void test_hdd_attach_still_works(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char path[300];
    fixture_path(path, sizeof(path), "scsi_cdrom_test_hdd.HDS");
    write_fake_scsi_image(path);

    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(b->spc_in.units[0].kind == SCSI_UNIT_DISK);
    assert(b->spc_in.units[0].write_protected == 0);
    assert(b->spc_in.units[0].bytes_per_record == 512);
    assert(b->spc_in.units[0].disk_end_record == 16);

    x68k_bus_scsi_eject(b, 0);
    unlink(path);
    printf("test_hdd_attach_still_works: OK\n");
}

/* attach失敗時(不正な.ccd)は既存のメディアを失わない。 */
static void test_attach_failure_keeps_existing_media(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char good_path[300], bad_path[300];
    fixture_path(good_path, sizeof(good_path), "scsi_cdrom_test_good.HDS");
    fixture_path(bad_path, sizeof(bad_path), "scsi_cdrom_test_bad.ccd");
    write_fake_scsi_image(good_path);
    assert(x68k_bus_scsi_insert(b, 0, good_path) == 0);

    /* トラックエントリが無い(=disk_end_record確定不可)不正な.ccd。 */
    FILE *fp = fopen(bad_path, "wb");
    assert(fp != NULL);
    fprintf(fp, "[CloneCD]\r\nVersion=3\r\n[Disc]\r\nDataTracksScrambled=0\r\n");
    fclose(fp);

    assert(x68k_bus_scsi_insert(b, 0, bad_path) == -1);
    assert(x68k_bus_scsi_disk_path(b, 0) != NULL);
    assert(strcmp(x68k_bus_scsi_disk_path(b, 0), good_path) == 0);
    assert(b->spc_in.units[0].kind == SCSI_UNIT_DISK);

    x68k_bus_scsi_eject(b, 0);
    unlink(good_path);
    unlink(bad_path);
    printf("test_attach_failure_keeps_existing_media: OK\n");
}

/* COMMANDフェーズを10バイトCDBで駆動し、STATUSフェーズの状態バイトを読む
 * （READ TOC等group4x0オペコード=CDB長10用）。 */
static uint8_t drive_command10_and_get_status(x68k_bus_t *b, const uint8_t cdb[10]) {
    for (int i = 0; i < 10; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]);
        x68k_bus_write8(b, 0xE96025u, 0xE0u);
        x68k_bus_write8(b, 0xE96025u, 0xC0u);
    }
    return x68k_bus_read8(b, 0xE96037u);
}

/* COMMANDフェーズを10バイトCDBで駆動し、DATA_INフェーズからnバイト読み出す。 */
static void drive_command10_and_read_data(x68k_bus_t *b, const uint8_t cdb[10], int n,
                                          uint8_t *out) {
    for (int i = 0; i < 10; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]);
        x68k_bus_write8(b, 0xE96025u, 0xE0u);
        x68k_bus_write8(b, 0xE96025u, 0xC0u);
    }
    x68k_bus_write8(b, 0xE96039u, (uint8_t)(n >> 16)); /* TCH */
    x68k_bus_write8(b, 0xE9603Bu, (uint8_t)(n >> 8));  /* TCM */
    x68k_bus_write8(b, 0xE9603Du, (uint8_t)n);         /* TCL */
    x68k_bus_write8(b, 0xE96031u, 0x81u);              /* PCTL: DATA IN + Busfree INT */
    x68k_bus_write8(b, 0xE96025u, 0x80u);              /* SCMD: Transfer */
    for (int i = 0; i < n; i++)
        out[i] = x68k_bus_read8(b, 0xE96035u); /* DREG */
}

/* P2: InquiryのperipheralデバイスタイプがCD-ROM=0x05を返す。ベンダIDは変えない。 */
static void test_cdrom_inquiry_device_type(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_inq.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_inq.img");
    write_fixture_ccd(ccd_path, 4);
    write_fixture_img(img_path, 4);
    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);

    select_target(b, 0);
    uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0}; /* Inquiry, alloc=36 */
    uint8_t data[36];
    drive_command_and_read_data(b, cdb, 36, data);
    assert((data[0] & 0x1F) == 0x05);
    assert(memcmp(data + 8, "X68000  ", 8) == 0); /* ベンダIDは種別に関わらず変えない */

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_inquiry_device_type: OK\n");
}

/* CDROM.XはROMの_S_INQUIRYへalloc=92を渡す。内蔵ROMはその値をDMAC転送
 * カウントに使うため、標準応答36バイト以降も要求長までゼロ埋めして返す。 */
static void test_cdrom_inquiry_pads_to_allocation_length(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_inq92.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_inq92.img");
    write_fixture_ccd(ccd_path, 4);
    write_fixture_img(img_path, 4);
    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);

    select_target(b, 0);
    uint8_t cdb[6] = {0x12, 0, 0, 0, 92, 0};
    uint8_t data[92];
    memset(data, 0xA5, sizeof(data));
    drive_command_and_read_data(b, cdb, sizeof(data), data);
    assert((data[0] & 0x1F) == 0x05);
    for (size_t i = 36; i < sizeof(data); i++)
        assert(data[i] == 0);

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_inquiry_pads_to_allocation_length: OK\n");
}

/* P2回帰: HDDのInquiryは従来通りperipheral device type=0x00のまま。 */
static void test_hdd_inquiry_device_type_unchanged(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char path[300];
    fixture_path(path, sizeof(path), "scsi_cdrom_test_hdd_inq.HDS");
    write_fake_scsi_image(path);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
    uint8_t data[36];
    drive_command_and_read_data(b, cdb, 36, data);
    assert((data[0] & 0x1F) == 0x00);

    x68k_bus_scsi_eject(b, 0);
    unlink(path);
    printf("test_hdd_inquiry_device_type_unchanged: OK\n");
}

/* P2: Mode Sense(6)がwrite_protectedユニットでWPビット(byte[2] bit7)を立てる。 */
static void test_cdrom_mode_sense_write_protect_bit(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_ms.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_ms.img");
    write_fixture_ccd(ccd_path, 4);
    write_fixture_img(img_path, 4);
    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);
    clear_unit_attention(b, 0);

    select_target(b, 0);
    uint8_t cdb[6] = {0x1A, 0, 0x3F, 0, 12, 0}; /* Mode Sense(6), page=0x3F, alloc=12 */
    uint8_t data[12];
    drive_command_and_read_data(b, cdb, 12, data);
    assert((data[2] & 0x80) != 0);

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_mode_sense_write_protect_bit: OK\n");
}

/* P2: 書き込み系コマンド(Write(6)/Format Unit)はDATA PROTECT(sense key 0x07)で拒否する。 */
static void test_cdrom_write_rejected_with_data_protect(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_wp.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_wp.img");
    write_fixture_ccd(ccd_path, 4);
    write_fixture_img(img_path, 4);
    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);
    clear_unit_attention(b, 0);

    select_target(b, 0);
    uint8_t write6_cdb[6] = {0x0A, 0, 0, 0, 1, 0};               /* Write(6) LBA=0 n=1 */
    assert(drive_command_and_get_status(b, write6_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[2] == 0x07); /* DATA PROTECT */

    select_target(b, 0);
    uint8_t format_cdb[6] = {0x04, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, format_cdb) == 0x02); /* CHECK CONDITION */

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_write_rejected_with_data_protect: OK\n");
}

/* P3: READ TOC(MSF=0)がトラック1+リードアウトの2ディスクリプタを正しく返す。
 * adr_control==0x16、トラック1のLBA=0、リードアウトLBA=leadoutを確認。 */
static void test_cdrom_read_toc_lba(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_toc.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_toc.img");
    const uint32_t leadout = 4;
    write_fixture_ccd(ccd_path, leadout);
    write_fixture_img(img_path, leadout);
    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);
    clear_unit_attention(b, 0);

    select_target(b, 0);
    /* CDB: 43 00(MSF=0) 00 00 00 00 <start=0> <alloc_hi> <alloc_lo> 00 */
    uint8_t cdb[10] = {0x43, 0x00, 0, 0, 0, 0, 0x00, 0x00, 20, 0};
    uint8_t data[20];
    drive_command10_and_read_data(b, cdb, 20, data);

    assert(data[0] == 0x00 && data[1] == 18); /* データ長 = response_len(20) - 2 */
    assert(data[2] == 1);                     /* first track */
    assert(data[3] == 1);                     /* last track */
    /* descriptor[0]: トラック1 */
    assert(data[4 + 1] == 0x16); /* adr_control */
    assert(data[4 + 2] == 1);    /* track_no */
    assert(data[4 + 4] == 0 && data[4 + 5] == 0 && data[4 + 6] == 0 && data[4 + 7] == 0);
    /* descriptor[1]: リードアウト */
    assert(data[12 + 1] == 0x16);
    assert(data[12 + 2] == SCSI_CD_LEADOUT_TRACK);
    uint32_t leadout_lba = ((uint32_t)data[12 + 4] << 24) | ((uint32_t)data[12 + 5] << 16) |
                           ((uint32_t)data[12 + 6] << 8) | data[12 + 7];
    assert(leadout_lba == leadout);

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_read_toc_lba: OK\n");
}

/* P3: READ TOC(MSF=1)がLBA→M/S/F変換を正しく行う。トラック1(LBA0)=00:02:00、
 * リードアウト(LBA4)=00:02:04。 */
static void test_cdrom_read_toc_msf(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_toc_msf.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_toc_msf.img");
    write_fixture_ccd(ccd_path, 4);
    write_fixture_img(img_path, 4);
    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);
    clear_unit_attention(b, 0);

    select_target(b, 0);
    uint8_t cdb[10] = {0x43, 0x02, 0, 0, 0, 0, 0x00, 0x00, 20, 0}; /* MSF=1 */
    uint8_t data[20];
    drive_command10_and_read_data(b, cdb, 20, data);

    assert(data[4 + 4] == 0); /* トラック1: 00:02:00 */
    assert(data[4 + 5] == 0 && data[4 + 6] == 2 && data[4 + 7] == 0);
    assert(data[12 + 4] == 0); /* リードアウト: 00:02:04 */
    assert(data[12 + 5] == 0 && data[12 + 6] == 2 && data[12 + 7] == 4);

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_read_toc_msf: OK\n");
}

/* P3: start_track=0xAAはリードアウトのディスクリプタ1件のみを返す。 */
static void test_cdrom_read_toc_leadout_only(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_toc_lo.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_toc_lo.img");
    write_fixture_ccd(ccd_path, 4);
    write_fixture_img(img_path, 4);
    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);
    clear_unit_attention(b, 0);

    select_target(b, 0);
    uint8_t cdb[10] = {0x43, 0x00, 0, 0, 0, 0, 0xAA, 0x00, 12, 0};
    uint8_t data[12];
    drive_command10_and_read_data(b, cdb, 12, data);

    assert(data[0] == 0x00 && data[1] == 10); /* response_len(12) - 2 */
    assert(data[4 + 2] == SCSI_CD_LEADOUT_TRACK);

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_read_toc_leadout_only: OK\n");
}

/* P3: Allocation Length境界。alloc<response_lenは切り詰め、alloc>response_len
 * (SCSI_DATA_BUF_SIZEを含む)でもバッファを超えず安全に完結する
 * （scsi_design.md §6）。 */
static void test_cdrom_read_toc_allocation_length_bounds(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_toc_alloc.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_toc_alloc.img");
    write_fixture_ccd(ccd_path, 4);
    write_fixture_img(img_path, 4);
    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);
    clear_unit_attention(b, 0);

    /* response_len = 4 + 2*8 = 20。alloc=8(切り詰め): ヘッダのみ読める。 */
    select_target(b, 0);
    uint8_t cdb_small[10] = {0x43, 0x00, 0, 0, 0, 0, 0x00, 0x00, 8, 0};
    uint8_t data_small[8];
    drive_command10_and_read_data(b, cdb_small, 8, data_small);
    assert(data_small[0] == 0x00 && data_small[1] == 18); /* ヘッダは完全な応答長を示す */

    /* alloc=0xFFFF(SCSI_DATA_BUF_SIZE=1024を大幅に超過)でもtransfer_lenは
     * response_len(20)に頭打ちされ、クラッシュせず完結する。 */
    select_target(b, 0);
    uint8_t cdb_large[10] = {0x43, 0x00, 0, 0, 0, 0, 0x00, 0xFF, 0xFF, 0};
    uint8_t data_large[20];
    drive_command10_and_read_data(b, cdb_large, 20, data_large);
    assert(data_large[0] == 0x00 && data_large[1] == 18);
    assert(data_large[4 + 2] == 1); /* トラック1が正しく読める */

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_read_toc_allocation_length_bounds: OK\n");
}

/* P3回帰: HDDユニットへのREAD TOCはILLEGAL REQUESTで拒否する。 */
static void test_hdd_read_toc_rejected(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char path[300];
    fixture_path(path, sizeof(path), "scsi_cdrom_test_hdd_toc.HDS");
    write_fake_scsi_image(path);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    uint8_t cdb[10] = {0x43, 0x00, 0, 0, 0, 0, 0x00, 0x00, 20, 0};
    assert(drive_command10_and_get_status(b, cdb) == 0x02); /* CHECK CONDITION */

    x68k_bus_scsi_eject(b, 0);
    unlink(path);
    printf("test_hdd_read_toc_rejected: OK\n");
}

/* リムーバブルメディアが排出を経ずに別メディアへ差し替わった場合でも、
 * メディア挿入済み状態でattention_pendingが立っていればUNIT ATTENTIONを返す。
 * TEST UNIT READY(CHECK CONDITION)→REQUEST SENSE(key=0x06)→再試行TEST UNIT
 * READY(GOOD)の流れを確認する（mint.xがこの流れでメディア交換を検知する）。 */
static void test_cdrom_media_swap_reports_unit_attention(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path_a[300], img_path_a[300], ccd_path_b[300], img_path_b[300];
    fixture_path(ccd_path_a, sizeof(ccd_path_a), "scsi_cdrom_test_swap_a.ccd");
    fixture_path(img_path_a, sizeof(img_path_a), "scsi_cdrom_test_swap_a.img");
    fixture_path(ccd_path_b, sizeof(ccd_path_b), "scsi_cdrom_test_swap_b.ccd");
    fixture_path(img_path_b, sizeof(img_path_b), "scsi_cdrom_test_swap_b.img");
    write_fixture_ccd(ccd_path_a, 4);
    write_fixture_img(img_path_a, 4);
    write_fixture_ccd(ccd_path_b, 6);
    write_fixture_img(img_path_b, 6);

    assert(x68k_bus_scsi_insert(b, 0, ccd_path_a) == 0);
    clear_unit_attention(b, 0);

    /* 排出(scsi_detach())を挟まず、scsi_attach()の上書きだけで直接別メディアへ
     * 差し替える（イジェクト→即挿入がSUSIE.Xのポーリング間隔に収まった場合の再現）。 */
    assert(x68k_bus_scsi_insert(b, 0, ccd_path_b) == 0);

    select_target(b, 0);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[2] == 0x06); /* UNIT ATTENTION */

    select_target(b, 0);
    assert(drive_command_and_get_status(b, tur_cdb) == 0x00); /* 再試行はGOOD */

    x68k_bus_scsi_eject(b, 0);
    unlink(ccd_path_a);
    unlink(img_path_a);
    unlink(ccd_path_b);
    unlink(img_path_b);
    printf("test_cdrom_media_swap_reports_unit_attention: OK\n");
}

/* disk windowのEJECTボタン相当(scsi_detach())を押した場合の回帰。
 * 排出直後、メディア無し状態でのTest Unit ReadyがUNIT ATTENTIONを返すことを
 * 確認する（修正前は「メディア無しのTest Unit ReadyはXEiJ準拠で常にGOOD」という
 * ロジックがattention_pendingより優先され、排出がホストに全く伝わらなかった）。
 * 消費後の2回目はNOT READYになることも確認する。 */
static void test_cdrom_eject_reports_unit_attention(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char ccd_path[300], img_path[300];
    fixture_path(ccd_path, sizeof(ccd_path), "scsi_cdrom_test_eject.ccd");
    fixture_path(img_path, sizeof(img_path), "scsi_cdrom_test_eject.img");
    write_fixture_ccd(ccd_path, 4);
    write_fixture_img(img_path, 4);

    assert(x68k_bus_scsi_insert(b, 0, ccd_path) == 0);
    clear_unit_attention(b, 0);

    x68k_bus_scsi_eject(b, 0);                      /* disk windowのEJECTボタン相当 */
    assert(x68k_bus_scsi_unit_present(b, 0) == 1);  /* リムーバブルはドライブが残る */
    assert(x68k_bus_scsi_unit_attached(b, 0) == 0); /* メディアは無い */

    select_target(b, 0);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[2] == 0x06); /* UNIT ATTENTION */

    /* 消費後の再試行はメディア無し継続中のNOT READYになる
     * （Test Unit Ready自体はXEiJ準拠でGOODを返す既存仕様のまま）。 */
    select_target(b, 0);
    assert(drive_command_and_get_status(b, tur_cdb) == 0x00); /* GOOD(XEiJ準拠) */

    select_target(b, 0);
    uint8_t read_cdb[6] = {0x08, 0, 0, 0, 1, 0};
    assert(drive_command_and_get_status(b, read_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense2[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense2);
    assert(sense2[2] == 0x02); /* NOT READY */

    unlink(ccd_path);
    unlink(img_path);
    printf("test_cdrom_eject_reports_unit_attention: OK\n");
}

/* 空CD-ROMドライブ(cfg `scsi_diskN=CD`用)はメディア無しでもドライブとして存在する。 */
static void test_empty_cdrom_drive(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    assert(x68k_bus_scsi_unit_present(b, 3) == 0);
    assert(x68k_bus_scsi_insert_empty_cdrom(b, 3) == 0);
    assert(x68k_bus_scsi_unit_present(b, 3) == 1);
    assert(x68k_bus_scsi_unit_attached(b, 3) == 0); /* メディアは無い */
    assert(b->spc_in.units[3].kind == SCSI_UNIT_CDROM);
    assert(b->spc_in.units[3].write_protected == 1);

    printf("test_empty_cdrom_drive: OK\n");
}

/* x68k_bus_scsi_unit_is_cdrom()がkindを正しく反映する
 * （disk_window.cの空メディア表示分岐・ファイルダイアログ拡張子選択に使う）。 */
static void test_scsi_unit_is_cdrom_accessor(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    assert(x68k_bus_scsi_insert_empty_cdrom(b, 0) == 0);
    assert(x68k_bus_scsi_unit_is_cdrom(b, 0) == 1);

    assert(x68k_bus_scsi_insert_empty_removable(b, 1) == 0);
    assert(x68k_bus_scsi_unit_is_cdrom(b, 1) == 0);

    char path[300];
    fixture_path(path, sizeof(path), "scsi_cdrom_test_is_cdrom.HDS");
    write_fake_scsi_image(path);
    assert(x68k_bus_scsi_insert(b, 2, path) == 0);
    assert(x68k_bus_scsi_unit_is_cdrom(b, 2) == 0);
    unlink(path);

    assert(x68k_bus_scsi_unit_is_cdrom(b, 6) == 0);    /* 未接続ユニット */
    assert(x68k_bus_scsi_unit_is_cdrom(NULL, 0) == 0); /* NULL bus */
    assert(x68k_bus_scsi_unit_is_cdrom(b, -1) == 0);   /* 範囲外id */

    printf("test_scsi_unit_is_cdrom_accessor: OK\n");
}

int main(void) {
    test_cdrom_attach_parses_ccd();
    test_cdrom_read_extracts_user_data();
    test_cdrom_rejects_scrambled();
    test_cdrom_rejects_non_mode1();
    test_cdrom_truncated_img_uses_actual_size();
    test_hdd_attach_still_works();
    test_attach_failure_keeps_existing_media();
    test_cdrom_inquiry_device_type();
    test_cdrom_inquiry_pads_to_allocation_length();
    test_hdd_inquiry_device_type_unchanged();
    test_cdrom_mode_sense_write_protect_bit();
    test_cdrom_write_rejected_with_data_protect();
    test_cdrom_read_toc_lba();
    test_cdrom_read_toc_msf();
    test_cdrom_read_toc_leadout_only();
    test_cdrom_read_toc_allocation_length_bounds();
    test_hdd_read_toc_rejected();
    test_cdrom_media_swap_reports_unit_attention();
    test_cdrom_eject_reports_unit_attention();
    test_empty_cdrom_drive();
    test_scsi_unit_is_cdrom_accessor();
    printf("scsi_cdrom_test: all tests passed\n");
    return 0;
}
