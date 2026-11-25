/* SCSI MO(リムーバブルメディア)ライトプロテクト(WP)の単体テスト。
 * 設計: docs/ix68k-internals/design/scsi_design.md §7 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* win_get_temp_path()/access()/chmod()/S_IWUSR相当 */
#include <io.h>
#define unlink _unlink
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* scsi_attach()が要求する最小のX68SCSI1ヘッダ(16B)を持つ一時イメージを作る。
 * blen=512, blocks=16(適当な小容量)。removable!=0でEJ=1(MO相当)にする。 */
static void write_fake_scsi_image(const char *path, int removable) {
    /* 前回実行がWP系assert失敗でabort()した場合、後始末（末尾のchmod+unlink）を
     * 経由せず読み取り専用の一時ファイルが残ることがある（"wb"でのfopen自体が
     * 失敗し道連れで落ちる）。書き込み前に無条件でchmod+removeし、
     * 失敗（未作成でENOENT等）は無視して自己修復する。 */
    chmod(path, 0644);
    remove(path);
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
    hdr[0x0F] = (uint8_t)(removable ? 0x01 : 0x00); /* EJ */
    fwrite(hdr, 1, sizeof(hdr), fp);
    uint8_t zero[512] = {0};
    for (int i = 0; i < 16; i++)
        fwrite(zero, 1, sizeof(zero), fp);
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

/* ---- SCSI(SPC)コマンド駆動ヘルパ(scsi_status_test.cと同じ手順) ---- */

static void select_target(x68k_bus_t *b, int id) {
    x68k_bus_write8(b, 0xE96037u, (uint8_t)(1u << id)); /* TEMP: 対象ID */
    x68k_bus_write8(b, 0xE96025u, 0x20u);               /* SCMD: Select */
}

static uint8_t drive_command_and_get_status(x68k_bus_t *b, const uint8_t cdb[6]) {
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]); /* TEMP: 送信バイト */
        x68k_bus_write8(b, 0xE96025u, 0xE0u);  /* SCMD: Set ACK/REQ */
        x68k_bus_write8(b, 0xE96025u, 0xC0u);  /* SCMD: Reset ACK/REQ */
    }
    return x68k_bus_read8(b, 0xE96037u); /* TEMP: STATUSフェーズの状態バイト */
}

static void drive_command_and_read_data(x68k_bus_t *b, const uint8_t cdb[6], int n,
                                        uint8_t *out) {
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]);
        x68k_bus_write8(b, 0xE96025u, 0xE0u);
        x68k_bus_write8(b, 0xE96025u, 0xC0u);
    }
    x68k_bus_write8(b, 0xE96039u, 0);          /* TCH */
    x68k_bus_write8(b, 0xE9603Bu, 0);          /* TCM */
    x68k_bus_write8(b, 0xE9603Du, (uint8_t)n); /* TCL */
    x68k_bus_write8(b, 0xE96031u, 0x81u);      /* PCTL: DATA IN + Busfree INT */
    x68k_bus_write8(b, 0xE96025u, 0x80u);      /* SCMD: Transfer */
    for (int i = 0; i < n; i++)
        out[i] = x68k_bus_read8(b, 0xE96035u); /* DREG */
}

/* Write(6): LBA=0, n=1。1ブロック(512B)分をDATA_OUTで送る。 */
static uint8_t drive_write6(x68k_bus_t *b, const uint8_t data[512]) {
    const uint8_t cdb[6] = {0x0A, 0x00, 0x00, 0x00, 0x01, 0x00};
    for (int i = 0; i < 6; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]);
        x68k_bus_write8(b, 0xE96025u, 0xE0u);
        x68k_bus_write8(b, 0xE96025u, 0xC0u);
    }
    /* CHECK CONDITION(拒否)ならこの時点でSTATUSフェーズに入っている。
     * GOOD(受理)ならDATA_OUTフェーズへ進んでいるので512Bを送る。 */
    uint8_t psns = x68k_bus_read8(b, 0xE9602Bu); /* PSNS */
    if ((psns & 0x07u) == 0x00u) { /* DATA_OUT */
        x68k_bus_write8(b, 0xE96039u, 0);
        x68k_bus_write8(b, 0xE9603Bu, 2);
        x68k_bus_write8(b, 0xE9603Du, 0);
        x68k_bus_write8(b, 0xE96031u, 0x82u);  /* PCTL: COMMAND/DATA OUT + BF INT */
        x68k_bus_write8(b, 0xE96025u, 0x80u);  /* SCMD: Transfer */
        for (int i = 0; i < 512; i++)
            x68k_bus_write8(b, 0xE96035u, data[i]); /* DREG */
    }
    return x68k_bus_read8(b, 0xE96037u); /* TEMP: STATUSフェーズの状態バイト */
}

/* ---- T0/T1/T2/T3/T4/T4': attach_hdd()のWP自動判定・rbフォールバック ---- */

/* T0: EJ=1(removable)の0444イメージがinsert成功すること。 */
static void test_t0_readonly_mo_insert_succeeds(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t0_readonly_mo_insert_succeeds: SKIP (root)\n");
        return;
    }
#endif
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t0.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0444);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    chmod(path, 0644);
    unlink(path);
    printf("test_t0_readonly_mo_insert_succeeds: OK\n");
}

/* T1: EJ=1(removable)・0644のイメージ挿入 → write_protected()==0。 */
static void test_t1_writable_mo_not_protected(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t1.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0644);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 0);

    unlink(path);
    printf("test_t1_writable_mo_not_protected: OK\n");
}

/* T2: 0444のMOを挿入 → write_protected()==1。rootはスキップ(access(W_OK)が
 * 0444でも成功するため)。 */
static void test_t2_readonly_mo_is_protected(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t2_readonly_mo_is_protected: SKIP (root)\n");
        return;
    }
#endif
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t2.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0444);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 1);

    chmod(path, 0644);
    unlink(path);
    printf("test_t2_readonly_mo_is_protected: OK\n");
}

/* T3: T2の状態からeject→同じ0644イメージを再挿入 → write_protected()==0
 * (イジェクトで既存ユニットが丸ごと置き換わり、再挿入時に再判定される)。 */
static void test_t3_wp_recomputed_on_reinsert(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t3_wp_recomputed_on_reinsert: SKIP (root)\n");
        return;
    }
#endif
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t3.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0444);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 1);

    x68k_bus_scsi_eject(b, 0);
    chmod(path, 0644);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 0);

    unlink(path);
    printf("test_t3_wp_recomputed_on_reinsert: OK\n");
}

/* T4: EJ=0(HDD、非removable)を0444で挿入 → 接続成功、
 * write_protected==1(rbフォールバックによる強制保護。HDD向けWP自動検出
 * ではなく、物理的に書けないファイルを書けると偽らないための安全策)。 */
static void test_t4_readonly_hdd_forces_protected(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t4_readonly_hdd_forces_protected: SKIP (root)\n");
        return;
    }
#endif
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t4.HDS");
    write_fake_scsi_image(path, 0);
    chmod(path, 0444);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 1);

    chmod(path, 0644);
    unlink(path);
    printf("test_t4_readonly_hdd_forces_protected: OK\n");
}

/* T4': EJ=0(HDD)を0644(通常)で挿入 → write_protected==0(退行なし)。 */
static void test_t4b_writable_hdd_not_protected(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t4b.HDS");
    write_fake_scsi_image(path, 0);
    chmod(path, 0644);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 0);

    unlink(path);
    printf("test_t4b_writable_hdd_not_protected: OK\n");
}

/* ---- T5-T9: SPCコマンド層でのWP拒否・退行なし ---- */

/* T5: WP状態でWrite(6) → CHECK CONDITION、センス[2]==0x07(DATA PROTECT)、
 * イメージ不変。この時点でunit->fpはrbで開かれているため、
 * spc_reject_write_protected()がDATA_OUT遷移前に必ず先着することの確認でもある。 */
static void test_t5_write6_rejected_when_protected(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t5_write6_rejected_when_protected: SKIP (root)\n");
        return;
    }
#endif
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t5.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0444);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 1);

    /* 挿入直後のUNIT ATTENTIONをTest Unit Readyで消費する。 */
    select_target(b, 0);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02); /* UNIT ATTENTION消費 */

    select_target(b, 0);
    uint8_t data[512];
    memset(data, 0xAA, sizeof(data));
    assert(drive_write6(b, data) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[0] == 0x70);
    assert(sense[2] == 0x07); /* DATA PROTECT */

    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    uint8_t body[512];
    assert(fseek(fp, 16, SEEK_SET) == 0);
    assert(fread(body, 1, sizeof(body), fp) == sizeof(body));
    fclose(fp);
    for (size_t i = 0; i < sizeof(body); i++)
        assert(body[i] == 0); /* 書き込まれていない */

    chmod(path, 0644);
    unlink(path);
    printf("test_t5_write6_rejected_when_protected: OK\n");
}

/* Write(10): LBA=0, n=1。10バイトCDB(group2)でDATA_OUTへ1ブロック分送る。 */
static uint8_t drive_write10(x68k_bus_t *b, const uint8_t data[512]) {
    const uint8_t cdb[10] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    for (int i = 0; i < 10; i++) {
        x68k_bus_write8(b, 0xE96037u, cdb[i]);
        x68k_bus_write8(b, 0xE96025u, 0xE0u);
        x68k_bus_write8(b, 0xE96025u, 0xC0u);
    }
    uint8_t psns = x68k_bus_read8(b, 0xE9602Bu);
    if ((psns & 0x07u) == 0x00u) { /* DATA_OUT */
        x68k_bus_write8(b, 0xE96039u, 0);
        x68k_bus_write8(b, 0xE9603Bu, 2);
        x68k_bus_write8(b, 0xE9603Du, 0);
        for (int i = 0; i < 512; i++)
            x68k_bus_write8(b, 0xE96035u, data[i]);
    }
    return x68k_bus_read8(b, 0xE96037u);
}

/* T6: WP状態でWrite(10) → T5と同じくCHECK CONDITION、DATA PROTECT、イメージ不変。 */
static void test_t6_write10_rejected_when_protected(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t6_write10_rejected_when_protected: SKIP (root)\n");
        return;
    }
#endif
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t6.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0444);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02); /* UNIT ATTENTION消費 */

    select_target(b, 0);
    uint8_t data[512];
    memset(data, 0xBB, sizeof(data));
    assert(drive_write10(b, data) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[2] == 0x07); /* DATA PROTECT */

    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    uint8_t body[512];
    assert(fseek(fp, 16, SEEK_SET) == 0);
    assert(fread(body, 1, sizeof(body), fp) == sizeof(body));
    fclose(fp);
    for (size_t i = 0; i < sizeof(body); i++)
        assert(body[i] == 0);

    chmod(path, 0644);
    unlink(path);
    printf("test_t6_write10_rejected_when_protected: OK\n");
}

/* T7: WP状態でFormat Unit → CHECK CONDITION、DATA PROTECT。 */
static void test_t7_format_unit_rejected_when_protected(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t7_format_unit_rejected_when_protected: SKIP (root)\n");
        return;
    }
#endif
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t7.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0444);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02); /* UNIT ATTENTION消費 */

    select_target(b, 0);
    const uint8_t format_cdb[6] = {0x04, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, format_cdb) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[2] == 0x07); /* DATA PROTECT */

    chmod(path, 0644);
    unlink(path);
    printf("test_t7_format_unit_rejected_when_protected: OK\n");
}

/* T8: WP状態でMode Sense(6) → device-specific parameter bit7(WP)が立つ。 */
static void test_t8_mode_sense_reports_wp_bit(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t8_mode_sense_reports_wp_bit: SKIP (root)\n");
        return;
    }
#endif
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t8.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0444);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02); /* UNIT ATTENTION消費 */

    select_target(b, 0);
    uint8_t mode_sense_cdb[6] = {0x1A, 0, 0, 0, 12, 0};
    uint8_t d[12] = {0};
    drive_command_and_read_data(b, mode_sense_cdb, 12, d);
    assert((d[2] & 0x80u) != 0);

    chmod(path, 0644);
    unlink(path);
    printf("test_t8_mode_sense_reports_wp_bit: OK\n");
}

/* T9: WP解除状態では従来どおり書ける(退行なし)。 */
static void test_t9_write6_succeeds_without_protection(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t9.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0644);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 0);

    select_target(b, 0);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02); /* UNIT ATTENTION消費 */

    select_target(b, 0);
    uint8_t data[512];
    memset(data, 0xAA, sizeof(data));
    assert(drive_write6(b, data) == 0x00); /* GOOD */

    /* scsi.cのWrite(6)はlba*bytes_per_recordを生ファイルオフセットとして
     * fseekする(ヘッダ分のオフセットは加算しない、scsi_unit_read()と対称)。
     * LBA0はファイル先頭(=X68SCSI1ヘッダの位置)を書き換える。 */
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    uint8_t body[512];
    assert(fseek(fp, 0, SEEK_SET) == 0);
    assert(fread(body, 1, sizeof(body), fp) == sizeof(body));
    fclose(fp);
    for (size_t i = 0; i < sizeof(body); i++)
        assert(body[i] == 0xAA);

    unlink(path);
    printf("test_t9_write6_succeeds_without_protection: OK\n");
}

/* ---- T10-T17: x68k_bus_scsi_set_write_protect() ---- */

/* T10/T11: トグルAPIでの往復がowner writeビットのみ操作し、group/otherを
 * 完全に保存すること。0664から開始する。 */
static void test_t10_t11_set_write_protect_round_trip(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t1011.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0664);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    assert(x68k_bus_scsi_set_write_protect(b, 0, 1) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 1);
#ifndef _WIN32
    /* owner writeビットのみがclearされ、group/otherは保持されること（本体側の方針を踏襲）。WindowsのNTFSにはowner/group/other粒度の
     * パーミッションが無く、win_compatのchmod/statシムは単一のread-only
     * 属性しか表現できないため、この厳密なビット単位の検証はPOSIXのみ
     * 行う（Windows側は直前・直後のwrite_protected()往復で十分検証済み）。 */
    struct stat st1;
    assert(stat(path, &st1) == 0);
    assert((st1.st_mode & 07777) == 0464);
#endif

    assert(x68k_bus_scsi_set_write_protect(b, 0, 0) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 0);
#ifndef _WIN32
    struct stat st2;
    assert(stat(path, &st2) == 0);
    assert((st2.st_mode & 07777) == 0664);
#endif

    chmod(path, 0644);
    unlink(path);
    printf("test_t10_t11_set_write_protect_round_trip: OK\n");
}

/* T12: 未挿入のremovable(空MO)へset_write_protect → NO_MEDIA。 */
static void test_t12_set_write_protect_no_media(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert_empty_removable(b, 0) == 0);
    assert(x68k_bus_scsi_set_write_protect(b, 0, 1) == X68K_SCSI_ERR_NO_MEDIA);
    printf("test_t12_set_write_protect_no_media: OK\n");
}

/* T13: HDD(非removable)へset_write_protect → NOT_SUPPORTED。
 * T4で強制write_protected=1になっていてもトグルAPI自体は拒否すること。 */
static void test_t13_set_write_protect_hdd_not_supported(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t13.HDS");
    write_fake_scsi_image(path, 0);
    chmod(path, 0644);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_set_write_protect(b, 0, 1) == X68K_SCSI_ERR_NOT_SUPPORTED);

    unlink(path);
    printf("test_t13_set_write_protect_hdd_not_supported: OK\n");
}

/* T14: CD-ROMユニットへset_write_protect → NOT_SUPPORTED。 */
static void test_t14_set_write_protect_cdrom_not_supported(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert_empty_cdrom(b, 0) == 0);
    assert(x68k_bus_scsi_set_write_protect(b, 0, 0) == X68K_SCSI_ERR_NOT_SUPPORTED);
    printf("test_t14_set_write_protect_cdrom_not_supported: OK\n");
}

/* T15: target_id==id(SELECT成立中)でset_write_protect → BUSY、
 * ファイル権限・フラグとも不変。 */
static void test_t15_set_write_protect_busy(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t15.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0644);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);

    select_target(b, 0);
    assert(x68k_bus_scsi_target_id(b) == 0);
    assert(x68k_bus_scsi_set_write_protect(b, 0, 1) == X68K_SCSI_ERR_BUSY);
    assert(x68k_bus_scsi_write_protected(b, 0) == 0);
#ifndef _WIN32
    /* ファイル権限が不変であることの厳密検証。Windows/NTFSでは表現できない
     * 粒度のためPOSIXのみ（test_t10_t11_set_write_protect_round_tripと同じ理由）。 */
    struct stat st;
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 07777) == 0644);
#endif

    unlink(path);
    printf("test_t15_set_write_protect_busy: OK\n");
}

/* T16: id範囲外(7, -1) → INVALID_ID。 */
static void test_t16_set_write_protect_invalid_id(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_set_write_protect(b, 7, 1) == X68K_SCSI_ERR_INVALID_ID);
    assert(x68k_bus_scsi_set_write_protect(b, -1, 1) == X68K_SCSI_ERR_INVALID_ID);
    printf("test_t16_set_write_protect_invalid_id: OK\n");
}

/* T17: 同一0644イメージをSCSI ID0/ID1両方へ挿入→set_write_protect(b,0,1)で
 * 両方とも再判定されること(D5手順7、WP迂回バグの回帰確認)。 */
static void test_t17_set_write_protect_syncs_sibling_units(void) {
    char path[300];
    temp_path(path, sizeof(path), "scsi_wp_test_t17.HDS");
    write_fake_scsi_image(path, 1);
    chmod(path, 0644);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_insert(b, 1, path) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 0);
    assert(x68k_bus_scsi_write_protected(b, 1) == 0);

    assert(x68k_bus_scsi_set_write_protect(b, 0, 1) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 1);
    assert(x68k_bus_scsi_write_protected(b, 1) == 1);

    x68k_bus_scsi_set_write_protect(b, 0, 0);
    chmod(path, 0644);
    unlink(path);
    printf("test_t17_set_write_protect_syncs_sibling_units: OK\n");
}

/* T18: メディア無し(空removable)でWrite(6) → 既存どおりCHECK CONDITION
 * (NOT READY)。WP判定に到達しないことの確認(D4)。 */
static void test_t18_write6_missing_media_returns_not_ready(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert_empty_removable(b, 0) == 0);

    select_target(b, 0);
    uint8_t tur_cdb[6] = {0x00, 0, 0, 0, 0, 0};
    assert(drive_command_and_get_status(b, tur_cdb) == 0x02); /* UNIT ATTENTION消費 */

    select_target(b, 0);
    uint8_t data[512] = {0};
    assert(drive_write6(b, data) == 0x02); /* CHECK CONDITION */

    select_target(b, 0);
    uint8_t sense_cdb[6] = {0x03, 0, 0, 0, 8, 0};
    uint8_t sense[8] = {0};
    drive_command_and_read_data(b, sense_cdb, 8, sense);
    assert(sense[2] == 0x02); /* NOT READY(WP判定より前に弾かれる) */

    printf("test_t18_write6_missing_media_returns_not_ready: OK\n");
}

/* T19: read-only固定HDDへ明示overlayを付けると、baseを変更せずguest writeを読戻せる。 */
static void test_t19_overlay_preserves_readonly_base(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t19_overlay_preserves_readonly_base: SKIP (root)\n");
        return;
    }
#endif
    char path[300], overlay[320];
    temp_path(path, sizeof(path), "scsi_wp_test_t19.HDS");
    temp_path(overlay, sizeof(overlay), "scsi_wp_test_t19.overlay");
    remove(overlay);
    write_fake_scsi_image(path, 0);
    chmod(path, 0444);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_scsi_insert(b, 0, path) == 0);
    assert(x68k_bus_scsi_enable_overlay(b, 0, overlay) == 0);
    assert(x68k_bus_scsi_write_protected(b, 0) == 0);
    uint8_t written[512], readback[512], base[512] = {0};
    memset(written, 0xA5, sizeof(written));
    select_target(b, 0);
    assert(drive_write6(b, written) == 0x00);
    const uint8_t read_cdb[6] = {0x08, 0, 0, 0, 1, 0};
    select_target(b, 0);
    drive_command_and_read_data(b, read_cdb, sizeof(readback), readback);
    assert(memcmp(readback, written, sizeof(written)) == 0);
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    assert(fseek(fp, 16, SEEK_SET) == 0);
    assert(fread(base, 1, sizeof(base), fp) == sizeof(base));
    fclose(fp);
    assert(memcmp(base, (uint8_t[512]){0}, sizeof(base)) == 0);
    x68k_bus_destroy(b);

    chmod(path, 0644);
    remove(path);
    remove(overlay);
    printf("test_t19_overlay_preserves_readonly_base: OK\n");
}

int main(void) {
    test_t0_readonly_mo_insert_succeeds();
    test_t1_writable_mo_not_protected();
    test_t2_readonly_mo_is_protected();
    test_t3_wp_recomputed_on_reinsert();
    test_t4_readonly_hdd_forces_protected();
    test_t4b_writable_hdd_not_protected();
    test_t5_write6_rejected_when_protected();
    test_t6_write10_rejected_when_protected();
    test_t7_format_unit_rejected_when_protected();
    test_t8_mode_sense_reports_wp_bit();
    test_t9_write6_succeeds_without_protection();
    test_t10_t11_set_write_protect_round_trip();
    test_t12_set_write_protect_no_media();
    test_t13_set_write_protect_hdd_not_supported();
    test_t14_set_write_protect_cdrom_not_supported();
    test_t15_set_write_protect_busy();
    test_t16_set_write_protect_invalid_id();
    test_t17_set_write_protect_syncs_sibling_units();
    test_t18_write6_missing_media_returns_not_ready();
    test_t19_overlay_preserves_readonly_base();
    printf("scsi_wp_test: all tests passed\n");
    return 0;
}
