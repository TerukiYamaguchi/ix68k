/* SCSIメディア状態、コマンド応答、公開アクセサの単体テスト。 */
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

/* プロセス一意な一時ディレクトリを初回呼び出し時にのみ作成し、以降はキャッシュを返す
 * （tests/fdc_eject_test.cと同じmkdtemp()/win_get_temp_template()パターン）。
 * 固定"/tmp/<ファイル名>"を複数プロセスが同時に読み書きして競合・abortする問題への対策。
 * テストバイナリはシングルスレッドのため排他制御は不要。 */
static const char *fixture_dir(void) {
    static char dir[300];
    static int initialized = 0;
    if (!initialized) {
#ifdef _WIN32
        win_get_temp_template(dir, sizeof(dir), "scsi_status_test.");
#else
        snprintf(dir, sizeof(dir), "/tmp/scsi_status_test.XXXXXX");
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

/* scsi_attach()が要求する最小のX68SCSI1ヘッダ(16B)を持つ一時イメージ
 * ファイルを作る。blen=512, blocks=16(適当な小容量)。 */
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
    /* blen*blocks分の本体(ゼロ埋め)を確保。 */
    uint8_t zero[512] = {0};
    for (int i = 0; i < 16; i++)
        fwrite(zero, 1, sizeof(zero), fp);
    fclose(fp);
}

static void test_disk_path_set_and_cleared(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_fake.HDS");
    write_fake_scsi_image(path);

    assert(x68k_bus_scsi_disk_path(b, 0) == NULL);
    assert(x68k_bus_scsi_unit_attached(b, 0) == 0);

    int err = x68k_bus_scsi_insert(b, 0, path);
    assert(err == 0);
    assert(x68k_bus_scsi_unit_attached(b, 0) == 1);
    assert(x68k_bus_scsi_disk_path(b, 0) != NULL);
    assert(strcmp(x68k_bus_scsi_disk_path(b, 0), path) == 0);
    assert(x68k_bus_scsi_disk_path(b, 1) == NULL); /* 他ユニットは無関係 */

    x68k_bus_scsi_eject(b, 0);
    assert(x68k_bus_scsi_unit_attached(b, 0) == 0);
    assert(x68k_bus_scsi_disk_path(b, 0) == NULL); /* イジェクトでファイル名も消える */

    printf("test_disk_path_set_and_cleared: OK\n");
}

static void test_reinsert_overwrites_path(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char path_a_buf[300], path_b_buf[300];
    const char *path_a = fixture_path(path_a_buf, sizeof(path_a_buf), "scsi_status_test_fake_a.HDS");
    const char *path_b = fixture_path(path_b_buf, sizeof(path_b_buf), "scsi_status_test_fake_b.HDS");
    write_fake_scsi_image(path_a);
    write_fake_scsi_image(path_b);

    assert(x68k_bus_scsi_insert(b, 2, path_a) == 0);
    assert(strcmp(x68k_bus_scsi_disk_path(b, 2), path_a) == 0);

    /* 再挿入時はメディアと表示用パスの両方を置き換える。 */
    assert(x68k_bus_scsi_insert(b, 2, path_b) == 0);
    assert(strcmp(x68k_bus_scsi_disk_path(b, 2), path_b) == 0);

    printf("test_reinsert_overwrites_path: OK\n");
}

static void test_target_id_and_transferring_defaults(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* リセット直後はバスフリー(未選択)・転送中でもない。 */
    assert(x68k_bus_scsi_target_id(b) == -1);
    assert(x68k_bus_scsi_transferring(b) == 0);

    printf("test_target_id_and_transferring_defaults: OK\n");
}

/* リセット直後のSSTSはアイドル値(TC0|DE=$05)。IPLROM 1.6は$E9602Dが$00/$FFだと
 * 内蔵SCSIなしと判断して"SCSIINROM is not required"で停止する。 */
static void test_ssts_idle_after_reset(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    assert(x68k_bus_read8(b, 0xE9602Du) == 0x05u);

    printf("test_ssts_idle_after_reset: OK\n");
}

/* SELECTフェーズを駆動する（SPC_BASE=$E96020、TEMP=+0x17、SCMD=+0x05、
 * SPC_SCMD_CC_SL=$20。src/peripheral/scsi.c参照）。bdidは既定でID7
 * （scsi_reset()のSPC_BDID_I7）のため明示的に書く必要はない。 */
static void select_target(x68k_bus_t *b, int id) {
    x68k_bus_write8(b, 0xE96037u, (uint8_t)(1u << id)); /* TEMP: 対象ID */
    x68k_bus_write8(b, 0xE96025u, 0x20u);               /* SCMD: Select */
}

/* COMMANDフェーズをSet/Reset ACK/REQ(SCMD=0xE0/0xC0)で駆動しCDBを送り込み、
 * コマンド実行後のSTATUSフェーズでTEMP(+0x17)に載る状態バイトを読む。
 * select_target()で選択済みのユニット宛てに使う。cdbは6バイト固定
 * (group0オペコード、SPC_CDB_LENGTH[0]==6)。 */
static uint8_t drive_command_and_get_status(x68k_bus_t *b, const uint8_t cdb[6]) {
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]); /* TEMP: 送信バイト */
        x68k_bus_write8(b, 0xE96025u, 0xE0u);  /* SCMD: Set ACK/REQ */
        x68k_bus_write8(b, 0xE96025u, 0xC0u);  /* SCMD: Reset ACK/REQ */
    }
    return x68k_bus_read8(b, 0xE96037u); /* TEMP: STATUSフェーズの状態バイト */
}

/* drive_command_and_get_status()と同様にCOMMANDフェーズを駆動するが、
 * DATA_INフェーズへ進むコマンド(Request Sense等)向けにDREG(+0x15)からnバイト
 * 読み出す。select_target()で選択済みのユニット宛てに使う。 */
static void drive_command_and_read_data(x68k_bus_t *b, const uint8_t cdb[6], int n,
                                        uint8_t *out) {
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]); /* TEMP: 送信バイト */
        x68k_bus_write8(b, 0xE96025u, 0xE0u);  /* SCMD: Set ACK/REQ */
        x68k_bus_write8(b, 0xE96025u, 0xC0u);  /* SCMD: Reset ACK/REQ */
    }
    /* DREG読み出しはtcが0になった時点で転送完了とみなす(scsi_read8_impl参照)。
     * 実ドライバはTransfer発行前にTCH:TCM:TCLへ転送バイト数を書くため、
     * ここでも先にtc=nをセットしないと1バイト目で完了と誤判定される。 */
    x68k_bus_write8(b, 0xE96039u, 0);          /* TCH */
    x68k_bus_write8(b, 0xE9603Bu, 0);          /* TCM */
    x68k_bus_write8(b, 0xE9603Du, (uint8_t)n); /* TCL */
    x68k_bus_write8(b, 0xE96031u, 0x81u);      /* PCTL: DATA IN + Busfree INT */
    x68k_bus_write8(b, 0xE96025u, 0x80u);      /* SCMD: Transfer */
    for (int i = 0; i < n; i++)
        out[i] = x68k_bus_read8(b, 0xE96035u); /* DREG */
}

/* 空リムーバブルは、メディア無しでもドライブとして存在する。 */
static void test_empty_removable_attach(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    assert(x68k_bus_scsi_unit_present(b, 3) == 0);
    assert(x68k_bus_scsi_insert_empty_removable(b, 3) == 0);
    assert(x68k_bus_scsi_unit_present(b, 3) == 1);
    assert(x68k_bus_scsi_unit_attached(b, 3) == 0); /* メディアは無い */
    assert(x68k_bus_scsi_disk_path(b, 3) == NULL);

    printf("test_empty_removable_attach: OK\n");
}

/* 空リムーバブルはSELECTに応答し、未接続IDはtarget_idを変更しない。 */
static void test_select_succeeds_for_empty_removable(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* id=2は誰も接続していないため、SELECTは失敗しtarget_idは-1のまま
     * （変化しない）。 */
    select_target(b, 2);
    assert(x68k_bus_scsi_target_id(b) == -1);

    assert(x68k_bus_scsi_insert_empty_removable(b, 1) == 0);
    select_target(b, 1);
    assert(x68k_bus_scsi_target_id(b) == 1);

    printf("test_select_succeeds_for_empty_removable: OK\n");
}

/* イジェクト後、リムーバブルはドライブが残り、固定ディスクは切断される。 */
static void test_detach_present_semantics(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* 空リムーバブルをイジェクト(no-op的だが、ドライブは残ったまま)。 */
    assert(x68k_bus_scsi_insert_empty_removable(b, 4) == 0);
    x68k_bus_scsi_eject(b, 4);
    assert(x68k_bus_scsi_unit_present(b, 4) == 1);
    assert(x68k_bus_scsi_unit_attached(b, 4) == 0);

    /* 固定ディスク(EJ=0)は通常のイジェクトでユニットごと消える。 */
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_fixed.HDS");
    write_fake_scsi_image(path); /* EJ=0 */
    assert(x68k_bus_scsi_insert(b, 5, path) == 0);
    assert(x68k_bus_scsi_unit_present(b, 5) == 1);
    x68k_bus_scsi_eject(b, 5);
    assert(x68k_bus_scsi_unit_present(b, 5) == 0);
    assert(x68k_bus_scsi_unit_attached(b, 5) == 0);

    printf("test_detach_present_semantics: OK\n");
}

/* XEiJ準拠のメディア不要コマンドはGOOD、それ以外はCHECK CONDITIONを返す。 */
static void test_missing_media_exempt_commands_return_good(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert_empty_removable(b, 6) == 0);

    /* 挿入直後の最初のコマンドはUNIT ATTENTIONを返すため、
     * Test Unit Readyで一度消費してからexemptコマンドの検証に入る。 */
    select_target(b, 6);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) ==
           0x02); /* CHECK CONDITION(UNIT ATTENTION) */

    uint8_t exempt_opcodes[3] = {0x00, 0x01, 0x1B};
    for (int i = 0; i < 3; i++) {
        select_target(b, 6);
        uint8_t cdb[6] = {exempt_opcodes[i], 0, 0, 0, 0, 0};
        assert(drive_command_and_get_status(b, cdb) == 0x00); /* GOOD */
    }

    select_target(b, 6);
    uint8_t read_cdb[6] = {0x08, 0, 0, 0, 1, 0};
    assert(drive_command_and_get_status(b, read_cdb) == 0x02); /* CHECK CONDITION */

    printf("test_missing_media_exempt_commands_return_good: OK\n");
}

/* UNIT ATTENTIONは接続直後の一度だけ報告し、その後はNOT READYを返す。 */
static void test_unit_attention_reported_once(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert_empty_removable(b, 6) == 0);

    /* 1回目: Read(6)(非例外、group0=6バイトCDB)がCHECK CONDITIONになり、
     * Request SenseはUNIT ATTENTION(0x06)を報告する。 */
    select_target(b, 6);
    uint8_t read_cap_cdb[6] = {0x08, 0, 0, 0, 1, 0};
    assert(drive_command_and_get_status(b, read_cap_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 6);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense1[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense1);
    assert(sense1[2] == 0x06); /* UNIT ATTENTION */

    /* 2回目: 未挿入状態は変わっていないのでCHECK CONDITIONは続くが、
     * Request SenseはもうNOT READY(0x02)を報告する。 */
    select_target(b, 6);
    assert(drive_command_and_get_status(b, read_cap_cdb) == 0x02);

    select_target(b, 6);
    uint8_t sense2[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense2);
    assert(sense2[2] == 0x02); /* NOT READY */

    printf("test_unit_attention_reported_once: OK\n");
}

/* 未実装のgroup 0 opcodeはCHECK CONDITIONとILLEGAL REQUESTを返す。 */
static void test_unimplemented_opcode_sets_illegal_request_sense(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_unimpl.HDS");
    write_fake_scsi_image(path);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    uint8_t unimpl_cdb[6] = {0x02, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, unimpl_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[0] == 0x70); /* 拡張センスフォーマット */
    assert(sense[2] == 0x05); /* ILLEGAL REQUEST */

    printf("test_unimplemented_opcode_sets_illegal_request_sense: OK\n");
}

/* Mode Sense(6)の未対応ページコードは、CHECK CONDITIONを返すだけでなく
 * ILLEGAL REQUEST / ASC=Invalid Field in CDB のセンスを残す。センス未設定だと
 * 続くRequest Senseが全ゼロを返し、イニシエータ側で不正なセンスとして扱われる
 * （NetBSDのsd(4)がpage 4/5を要求したときに発生していた）。 */
static void test_mode_sense_unsupported_page_sets_illegal_request_sense(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_msense.HDS");
    write_fake_scsi_image(path);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    /* page 4 = rigid disk geometry（未対応） */
    select_target(b, 0);
    uint8_t mode_sense_cdb[6] = {0x1A, 0x00, 0x04, 0x00, 12, 0x00};
    assert(drive_command_and_get_status(b, mode_sense_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, SCSI_SENSE_LENGTH, 0};
    uint8_t sense[SCSI_SENSE_LENGTH] = {0};
    drive_command_and_read_data(b, sense_cdb, SCSI_SENSE_LENGTH, sense);
    assert(sense[0] == 0x70);                            /* 拡張センスフォーマット */
    assert(sense[2] == 0x05);                            /* ILLEGAL REQUEST */
    assert(sense[7] == (uint8_t)(SCSI_SENSE_LENGTH - 8)); /* 追加センス長 */
    assert(sense[12] == 0x24);                           /* ASC: Invalid Field in CDB */
    assert(sense[13] == 0x00);                           /* ASCQ */

    /* 対応ページ(0x00)は従来どおりデータを返す。 */
    select_target(b, 0);
    uint8_t page0_cdb[6] = {0x1A, 0x00, 0x00, 0x00, 12, 0x00};
    uint8_t d[12] = {0};
    drive_command_and_read_data(b, page0_cdb, 12, d);
    assert(d[0] == 11); /* センスデータ長 */
    assert(d[3] == 8);  /* ブロックディスクリプタ長 */

    printf("test_mode_sense_unsupported_page_sets_illegal_request_sense: OK\n");
}

/* FORMAT UNITはGOODを返すno-opで、イメージを変更しない。 */
static void test_format_unit_is_noop(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_format.HDS");
    write_fake_scsi_image(path);
    FILE *fp = fopen(path, "r+b");
    assert(fp != NULL);
    const uint8_t marker[4] = {0xde, 0xad, 0xbe, 0xef};
    int seek_rc = fseek(fp, 512, SEEK_SET);
    assert(seek_rc == 0);
    size_t written = fwrite(marker, 1, sizeof(marker), fp);
    assert(written == sizeof(marker));
    fclose(fp);

    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    select_target(b, 0);
    const uint8_t format_cdb[6] = {0x04, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, format_cdb) == 0x00);

    fp = fopen(path, "rb");
    assert(fp != NULL);
    uint8_t actual[4];
    seek_rc = fseek(fp, 512, SEEK_SET);
    assert(seek_rc == 0);
    size_t read_count = fread(actual, 1, sizeof(actual), fp);
    assert(read_count == sizeof(actual));
    fclose(fp);
    assert(memcmp(actual, marker, sizeof(marker)) == 0);
    x68k_bus_scsi_eject(b, 0);
    unlink(path);
}

/* Seek(6)(0x0B): 範囲内LBAはGOOD、範囲外はILLEGAL REQUEST、
 * メディア無しは既存のmedia guard(280行付近)によりCHECK CONDITIONになる
 * （0x0Bはexemptリストに入れていないため）。 */
static void test_seek6_in_range_returns_good(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_seek_ok.HDS");
    write_fake_scsi_image(path); /* blocks=16 */
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    /* LBA=15(末尾ブロック): CDB[1]下位5bit=0,CDB[2]=0,CDB[3]=15 */
    uint8_t seek_cdb[6] = {0x0B, 0x00, 0x00, 0x0F, 0x00, 0x00};
    assert(drive_command_and_get_status(b, seek_cdb) == 0x00); /* GOOD */

    printf("test_seek6_in_range_returns_good: OK\n");
}

static void test_seek6_out_of_range_sets_illegal_request_sense(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_seek_oob.HDS");
    write_fake_scsi_image(path); /* blocks=16(disk_end_record=16) */
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    /* LBA=17(範囲外): CDB[3]=17 */
    uint8_t seek_cdb[6] = {0x0B, 0x00, 0x00, 0x11, 0x00, 0x00};
    assert(drive_command_and_get_status(b, seek_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[0] == 0x70); /* 拡張センスフォーマット */
    assert(sense[2] == 0x05); /* ILLEGAL REQUEST */

    printf("test_seek6_out_of_range_sets_illegal_request_sense: OK\n");
}

static void test_seek6_missing_media_returns_check_condition(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert_empty_removable(b, 6) == 0);

    /* 挿入直後のUNIT ATTENTIONをTest Unit Readyで消費してから検証する。 */
    select_target(b, 6);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02);

    select_target(b, 6);
    uint8_t seek_cdb[6] = {0x0B, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(drive_command_and_get_status(b, seek_cdb) == 0x02); /* CHECK CONDITION(NOT READY) */

    printf("test_seek6_missing_media_returns_check_condition: OK\n");
}

/* NetBSDはCDB byte 1 bit 7-5にもLUNを入れる。
 * SPCがLUN 1をLUN 0として扱わず、INQUIRYでは未対応LUNを報告すること、
 * その後の通常コマンドのCHECK CONDITIONを同じLUNのREQUEST SENSEで
 * 読み出せること、最後にLUN 0が引き続き使えることを確認する。 */
static void test_spc_nonzero_lun_is_not_lun0(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_spc_lun.HDS");
    write_fake_scsi_image(path);
    const uint8_t marker[4] = {0xde, 0xad, 0xbe, 0xef};
    FILE *fp = fopen(path, "r+b");
    assert(fp != NULL);
    assert(fseek(fp, 512, SEEK_SET) == 0);
    assert(fwrite(marker, 1, sizeof(marker), fp) == sizeof(marker));
    fclose(fp);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    /* LUN 1のINQUIRYはGOODで完了するが、byte 0=0x7fを返す。 */
    select_target(b, 0);
    const uint8_t inquiry_lun1[6] = {0x12, 0x20, 0x00, 0x00, 36, 0x00};
    uint8_t inquiry[36] = {0};
    drive_command_and_read_data(b, inquiry_lun1, (int)sizeof(inquiry), inquiry);
    assert(inquiry[0] == 0x7Fu);
    assert(x68k_bus_read8(b, 0xE96037u) == 0x00u); /* STATUS=GOOD */

    /* LUN 1の通常コマンドはLUN 0へ到達させずCHECK CONDITIONにする。 */
    select_target(b, 0);
    const uint8_t seek_lun1[6] = {0x0B, 0x20, 0x00, 0x00, 0x00, 0x00};
    assert(drive_command_and_get_status(b, seek_lun1) == 0x02u);

    /* REQUEST SENSEは同じLUN 1でも拒否せず、ILLEGAL REQUESTを返す。 */
    select_target(b, 0);
    const uint8_t sense_lun1[6] = {0x03, 0x20, 0x00, 0x00, 8, 0x00};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_lun1, (int)sizeof(sense), sense);
    assert(sense[0] == 0x70u);
    assert(sense[2] == 0x05u);

    /* LUN 1のWrite(6)はDATA OUT/pending_writeへ進む前に拒否され、
     * LUN 0のイメージ内容を変更しない。 */
    select_target(b, 0);
    const uint8_t write_lun1[6] = {0x0A, 0x20, 0x00, 0x00, 0x01, 0x00};
    assert(drive_command_and_get_status(b, write_lun1) == 0x02u);
    fp = fopen(path, "rb");
    assert(fp != NULL);
    uint8_t actual[sizeof(marker)];
    assert(fseek(fp, 512, SEEK_SET) == 0);
    assert(fread(actual, 1, sizeof(actual), fp) == sizeof(actual));
    fclose(fp);
    assert(memcmp(actual, marker, sizeof(marker)) == 0);

    /* LUN 0の既存経路は影響を受けない。 */
    select_target(b, 0);
    const uint8_t seek_lun0[6] = {0x0B, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(drive_command_and_get_status(b, seek_lun0) == 0x00u);

    printf("test_spc_nonzero_lun_is_not_lun0: OK\n");
}

/* 内蔵HDC(SASI互換I/F、$E96000、sasi.c)経由のSeek(6)テスト。
 * IOCS `_B_SEEK`(0x40)のPDA $80-$8FはSPC($E96020、scsi.c)ではなくこちらを
 * ROMが実際に叩く（$FF9D16のCDBが$E96001へ送られることをROM逆アセンブルで
 * 確認済み）。SEL($E96007)はSCSI/SASIと異なりビットマスクへの単発書き込み
 * のみで、Set/Reset ACK/REQのSCMDハンドシェイクは無い。 */
static void hdc_select(x68k_bus_t *b, int id) {
    x68k_bus_write8(b, 0xE96007u, (uint8_t)(1u << id));
}

static uint8_t hdc_command_and_get_status(x68k_bus_t *b, const uint8_t cdb[6]) {
    for (int i = 0; i < 6; i++)
        x68k_bus_write8(b, 0xE96001u, cdb[i]);
    return x68k_bus_read8(b, 0xE96001u); /* STATUSフェーズの状態バイト */
}

/* フェーズレジスタ($E96003)。BUS_FREE=0x00、COMMAND=0x0B(CD|BSY|REQ)、
 * DATA_IN=0x07、STATUS=0x0F、MESSAGE=0x1F。ROMもこの値を待つ。 */
static uint8_t hdc_phase_reg(x68k_bus_t *b) {
    return x68k_bus_read8(b, 0xE96003u);
}

/* STATUSに続くMESSAGEバイトを読み、バスをBUS_FREEへ戻す。1つのバスで複数
 * コマンドを続けて発行するテストでは、これを呼ばないと次のhdc_select()が
 * phase!=BUS_FREEガードで黙って無視される。 */
static uint8_t hdc_finish(x68k_bus_t *b) {
    return x68k_bus_read8(b, 0xE96001u);
}

/* CHECK CONDITION後にROMが行うのと同じ手順(再SELECT → 同じLUNのままREQUEST
 * SENSE STATUS(0x03)を発行 → DATA INで4バイト読む。IPLROMXV $FF9AD8-$FF9B0E)で
 * センスを読み出す。読了後のSTATUSバイトを*status_outへ返し、MESSAGEまで
 * 読んでBUS_FREEに戻す。 */
static void hdc_request_sense(x68k_bus_t *b, int id, uint8_t lun, uint8_t sense[4],
                              uint8_t *status_out) {
    hdc_select(b, id);
    const uint8_t cdb[6] = {0x03, (uint8_t)(lun << 5), 0x00, 0x00, 0x04, 0x00};
    for (int i = 0; i < 6; i++)
        x68k_bus_write8(b, 0xE96001u, cdb[i]);
    for (int i = 0; i < 4; i++)
        sense[i] = x68k_bus_read8(b, 0xE96001u); /* DATA IN */
    *status_out = x68k_bus_read8(b, 0xE96001u);  /* STATUS */
    (void)hdc_finish(b);                         /* MESSAGE → BUS_FREE */
}

static void test_hdc_seek6_in_range_returns_good(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_hdc_seek_ok.HDS");
    write_fake_scsi_image(path); /* blen=512,blocks=16 -> disk_bytes=8192 */
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    hdc_select(b, 0);
    /* LBA=31(256Bブロック単位。31*256=7936 <= disk_bytes=8192で範囲内) */
    uint8_t seek_cdb[6] = {0x0B, 0x00, 0x00, 0x1F, 0x00, 0x00};
    assert(hdc_command_and_get_status(b, seek_cdb) == 0x00); /* GOOD */

    printf("test_hdc_seek6_in_range_returns_good: OK\n");
}

static void test_hdc_seek6_out_of_range_returns_check_condition(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_hdc_seek_oob.HDS");
    write_fake_scsi_image(path); /* disk_bytes=8192、256Bブロックで上限32 */
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    hdc_select(b, 0);
    /* LBA=33(範囲外。33*256=8448 > disk_bytes=8192) */
    uint8_t seek_cdb[6] = {0x0B, 0x00, 0x00, 0x21, 0x00, 0x00};
    assert(hdc_command_and_get_status(b, seek_cdb) == 0x02); /* CHECK CONDITION */

    printf("test_hdc_seek6_out_of_range_returns_check_condition: OK\n");
}

/* 範囲外LBA・エラーセンス検証用のHDS(blen=512,blocks=16 → disk_bytes=8192)をID0へ挿入する。 */
static void hdc_insert_test_image(x68k_bus_t *b, const char *name) {
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), name);
    write_fake_scsi_image(path);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
}

/* T1/T3/T4: 空リムーバブル(present=1,fp=NULL)はSELECTに応答し
 * (D1)、コマンドはCHECK CONDITIONで終わり、REQUEST SENSEが$20を返す(D3/D4)。
 * 修正前はSELECTが成立せずphaseがBUS_FREEのままだった。 */
static void test_hdc_select_responds_to_empty_removable(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert_empty_removable(b, 1) == 0);

    /* T1: SELECTが成立しCOMMANDフェーズ(CD|BSY|REQ)へ入る。 */
    hdc_select(b, 1);
    assert(hdc_phase_reg(b) == 0x0Bu);

    /* T3: Seek(6) LUN=0 → メディア無しのためCHECK CONDITION、
     * MESSAGE読み出しでBUS_FREEへ戻る。 */
    const uint8_t seek_cdb[6] = {0x0B, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(hdc_command_and_get_status(b, seek_cdb) == 0x02u);
    assert(hdc_finish(b) == 0x00u);
    assert(hdc_phase_reg(b) == 0x00u); /* BUS_FREE */

    /* T4: 直後のREQUEST SENSEはINVALID COMMAND($20)を返す。 */
    uint8_t sense[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t status = 0xFF;
    hdc_request_sense(b, 1, 0, sense, &status);
    assert(sense[0] == 0x20u);
    assert(status == 0x00u); /* センス読み出し自体はGOOD */

    printf("test_hdc_select_responds_to_empty_removable: OK\n");
}

/* T2/T10: 未接続ID(present=0)はSELECTに応答せずBUS_FREEに留まる
 * （ROM側の5000回BSYポーリングでSELECTタイムアウトになる）。D1で無条件成立に
 * しなかったことの担保。 */
static void test_hdc_select_ignores_absent_unit(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* T2: 誰も接続していないID2。 */
    hdc_select(b, 2);
    assert(hdc_phase_reg(b) == 0x00u);

    /* T10: 固定ディスクはイジェクトでpresent=0になるため応答しなくなる。 */
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "scsi_status_test_hdc_fixed.HDS");
    write_fake_scsi_image(path); /* EJ=0(固定ディスク) */
    assert(x68k_bus_scsi_insert(b, 5, path) == 0);
    x68k_bus_scsi_eject(b, 5);
    assert(x68k_bus_scsi_unit_present(b, 5) == 0);
    hdc_select(b, 5);
    assert(hdc_phase_reg(b) == 0x00u);

    printf("test_hdc_select_ignores_absent_unit: OK\n");
}

/* T9: リムーバブルはイジェクト後もpresent=1が維持されるため
 * SELECTに応答する（scsi_media.cのdetachセマンティクスとの整合）。 */
static void test_hdc_select_responds_after_removable_eject(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert_empty_removable(b, 3) == 0);
    x68k_bus_scsi_eject(b, 3);
    assert(x68k_bus_scsi_unit_present(b, 3) == 1);

    hdc_select(b, 3);
    assert(hdc_phase_reg(b) == 0x0Bu);

    printf("test_hdc_select_responds_after_removable_eject: OK\n");
}

/* T5(動作再現ケース): PDA $81 は「SCSI ID1」ではなく
 * 「ID0のLUN1」で、IPLROMはLUNをCDB[1] bit5-7に載せる。修正前はLUNを見て
 * いなかったためID0の実HDDへSeekが通ってGOOD(D0=0)になっていた。 */
static void test_hdc_nonzero_lun_returns_check_condition(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    hdc_insert_test_image(b, "scsi_status_test_hdc_lun.HDS");

    hdc_select(b, 0);
    /* CDB[1]=0x20 → LUN=1。LBA部(下位5bit)は0で範囲内。 */
    const uint8_t seek_lun1_cdb[6] = {0x0B, 0x20, 0x00, 0x00, 0x00, 0x00};
    assert(hdc_command_and_get_status(b, seek_lun1_cdb) == 0x02u);
    assert(hdc_finish(b) == 0x00u);

    /* ROMはCHECK CONDITION後、同じLUN=1のままREQUEST SENSEを再発行する。 */
    uint8_t sense[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t status = 0xFF;
    hdc_request_sense(b, 0, 1, sense, &status);
    assert(sense[0] == 0x20u); /* INVALID COMMAND */
    assert(status == 0x00u);

    /* LUN=0の同じSeekは従来どおりGOOD（回帰確認）。 */
    hdc_select(b, 0);
    const uint8_t seek_lun0_cdb[6] = {0x0B, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(hdc_command_and_get_status(b, seek_lun0_cdb) == 0x00u);
    assert(hdc_finish(b) == 0x00u);

    printf("test_hdc_nonzero_lun_returns_check_condition: OK\n");
}

/* T6/T7: 範囲外LBAは$21(INVALID SECTOR ADDRESS)、未実装
 * オペコードは$20(INVALID COMMAND)をセンスに残す。修正前は常に$00だった。 */
static void test_hdc_error_paths_set_sense_code(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    hdc_insert_test_image(b, "scsi_status_test_hdc_sense.HDS");

    /* T6: LBA=33(33*256=8448 > disk_bytes=8192)。 */
    hdc_select(b, 0);
    const uint8_t seek_oob_cdb[6] = {0x0B, 0x00, 0x00, 0x21, 0x00, 0x00};
    assert(hdc_command_and_get_status(b, seek_oob_cdb) == 0x02u);
    assert(hdc_finish(b) == 0x00u);

    uint8_t sense[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t status = 0xFF;
    hdc_request_sense(b, 0, 0, sense, &status);
    assert(sense[0] == 0x21u); /* INVALID SECTOR ADDRESS */

    /* T7: 未実装オペコード WRITE(0x0A)。 */
    hdc_select(b, 0);
    const uint8_t write_cdb[6] = {0x0A, 0x00, 0x00, 0x00, 0x01, 0x00};
    assert(hdc_command_and_get_status(b, write_cdb) == 0x02u);
    assert(hdc_finish(b) == 0x00u);

    hdc_request_sense(b, 0, 0, sense, &status);
    assert(sense[0] == 0x20u); /* INVALID COMMAND */

    printf("test_hdc_error_paths_set_sense_code: OK\n");
}

/* T8(クリア順): センスは「返してからクリア」なので、2回続けて
 * REQUEST SENSEを発行すると1回目が直前のセンス、2回目は$00になる。修正前は
 * クリアが先で常に$00を返していた。 */
static void test_hdc_request_sense_clears_after_read(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    hdc_insert_test_image(b, "scsi_status_test_hdc_clear.HDS");

    hdc_select(b, 0);
    const uint8_t seek_oob_cdb[6] = {0x0B, 0x00, 0x00, 0x21, 0x00, 0x00};
    assert(hdc_command_and_get_status(b, seek_oob_cdb) == 0x02u);
    assert(hdc_finish(b) == 0x00u);

    uint8_t sense[4];
    uint8_t status;
    hdc_request_sense(b, 0, 0, sense, &status);
    assert(sense[0] == 0x21u);

    hdc_request_sense(b, 0, 0, sense, &status);
    assert(sense[0] == 0x00u); /* 読み出しでクリア済み */

    printf("test_hdc_request_sense_clears_after_read: OK\n");
}

int main(void) {
    test_disk_path_set_and_cleared();
    test_reinsert_overwrites_path();
    test_target_id_and_transferring_defaults();
    test_ssts_idle_after_reset();
    test_empty_removable_attach();
    test_select_succeeds_for_empty_removable();
    test_detach_present_semantics();
    test_missing_media_exempt_commands_return_good();
    test_unit_attention_reported_once();
    test_unimplemented_opcode_sets_illegal_request_sense();
    test_mode_sense_unsupported_page_sets_illegal_request_sense();
    test_format_unit_is_noop();
    test_seek6_in_range_returns_good();
    test_seek6_out_of_range_sets_illegal_request_sense();
    test_seek6_missing_media_returns_check_condition();
    test_spc_nonzero_lun_is_not_lun0();
    test_hdc_seek6_in_range_returns_good();
    test_hdc_seek6_out_of_range_returns_check_condition();
    test_hdc_select_responds_to_empty_removable();
    test_hdc_select_ignores_absent_unit();
    test_hdc_select_responds_after_removable_eject();
    test_hdc_nonzero_lun_returns_check_condition();
    test_hdc_error_paths_set_sense_code();
    test_hdc_request_sense_clears_after_read();
    printf("scsi_status_test: all tests passed\n");
    return 0;
}
