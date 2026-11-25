/*
 * $E94005 bit6(EJECT MASK)の単体テスト。物理イジェクトボタン相当の経路
 * （MCP eject_disk・GUIイジェクトボタン = x68k_bus_eject_fdc()）はマスク中
 * 無視されるが、IOCS _B_EJECT等のレジスタ書き込み経由(bit5 EJECT ON/OFF)は
 * ソフトウェア自身の制御経路のためマスクの対象外であることを確認する
 * （inside_x68000_1992.md 図5「1=イジェクトボタンを無効化する」）。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/win_compat.h" /* access()/chmod()/S_IWUSR相当（WPトグル用）・
                                   * win_get_temp_template()/mkdtemp()（プロセス一意な一時
                                   * ディレクトリ作成用） */
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* プロセス一意な一時ディレクトリを初回呼び出し時にのみ作成し、以降はキャッシュを返す
 * （tests/hostdrv_remote_test.cと同じmkdtemp()/win_get_temp_template()パターン）。
 * 固定"/tmp/<ファイル名>"を複数プロセスが同時に読み書きして競合・abortする問題への対策。
 *テストバイナリはシングルスレッドのため排他制御は不要。 */
static const char *fixture_dir(void) {
    static char dir[300];
    static int initialized = 0;
    if (!initialized) {
#ifdef _WIN32
        win_get_temp_template(dir, sizeof(dir), "fdc_eject_test.");
#else
        snprintf(dir, sizeof(dir), "/tmp/fdc_eject_test.XXXXXX");
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

/* fdc_eject_drive()はdisk[drive]の非NULLしか見ないため、実ファイル不要で
 * ダミーバッファを直接セットして「挿入中」を模擬する（mfp_fmirq_test.cと
 * 同じ直接フィールド操作の流儀）。 */
static void fake_insert(x68k_bus_t *b, int drive) {
    b->fdc.disk[drive] = malloc(1);
    assert(b->fdc.disk[drive] != NULL);
    b->fdc.disk_bytes[drive] = 1;
}

/* ドライブcontrol選択ビットを立ててから落とす(1→0エッジ)ことで、bit5-7の
 * オプション機能を発動させる。inside_x68000_1992.md「$23書込後に$20書込むと
 * ドライブ0・1が同時イジェクトされる」と同じプロトコル。 */
static void drv_ctrl_pulse(x68k_bus_t *b, uint8_t option_bits, int drive) {
    uint8_t sel = (uint8_t)(1u << drive);
    x68k_bus_write8(b, 0xE94005u, (uint8_t)(option_bits | sel));
    x68k_bus_write8(b, 0xE94005u, option_bits);
}

/* 1. EJECT MASK中は x68k_bus_eject_fdc()(MCP/GUI相当)が無視されること。 */
static void test_masked_button_eject_is_ignored(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    fake_insert(b, 0);

    drv_ctrl_pulse(b, 0x40u, 0); /* bit6=1(MASK) + drive0 選択→解除 */
    x68k_bus_eject_fdc(b, 0);
    assert(b->fdc.disk[0] != NULL); /* マスク中なので排出されない */
    printf("test_masked_button_eject_is_ignored: OK\n");
}

/* 2. マスク解除後は x68k_bus_eject_fdc() が効くこと。 */
static void test_unmasked_button_eject_works(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    fake_insert(b, 0);

    drv_ctrl_pulse(b, 0x40u, 0); /* マスク */
    drv_ctrl_pulse(b, 0x00u, 0); /* bit6=0(MASK解除) + drive0 選択→解除 */
    x68k_bus_eject_fdc(b, 0);
    assert(b->fdc.disk[0] == NULL); /* マスク解除済みなので排出される */
    printf("test_unmasked_button_eject_works: OK\n");
}

/* 3. マスク中でも、レジスタ書き込み経由(bit5 EJECT ON/OFF)の排出は
 *    対象外であること（ソフトウェア自身の制御経路）。 */
static void test_register_eject_bypasses_mask(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    fake_insert(b, 1);

    drv_ctrl_pulse(b, 0x40u, 1);       /* drive1をマスク */
    drv_ctrl_pulse(b, 0x20u, 1);       /* bit5(EJECT ON/OFF) + drive1 選択→解除 */
    assert(b->fdc.disk[1] == NULL);    /* マスクの影響を受けず排出される */
    printf("test_register_eject_bypasses_mask: OK\n");
}

/* 4. マスクはドライブ単位で独立していること。 */
static void test_mask_is_per_drive(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    fake_insert(b, 0);
    fake_insert(b, 1);

    drv_ctrl_pulse(b, 0x40u, 0); /* drive0のみマスク */
    x68k_bus_eject_fdc(b, 0);
    x68k_bus_eject_fdc(b, 1);
    assert(b->fdc.disk[0] != NULL); /* マスクされているので残る */
    assert(b->fdc.disk[1] == NULL); /* マスクされていないので排出される */
    printf("test_mask_is_per_drive: OK\n");
}

/* 5. FDC実行中(FDC_EXEC)はマスクの有無に関わらずx68k_bus_eject_fdc()が
 *    無視されること（UAF防止ガード）。fdc.c:298相当のdma_ptrがdisk[drive]
 *    バッファ内を指している状況を模擬する。 */
static void test_busy_button_eject_is_ignored(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    fake_insert(b, 0);

    b->fdc.phase = FDC_EXEC;
    b->fdc.dma_ptr = b->fdc.disk[0];
    b->fdc.dma_remaining = 1;
    b->fdc.dma_drv = 0;

    x68k_bus_eject_fdc(b, 0);
    assert(b->fdc.disk[0] != NULL); /* ビジー中なので排出されない */
    printf("test_busy_button_eject_is_ignored: OK\n");
}

/* 6. アイドルに戻れば通常通りイジェクトできること。 */
static void test_button_eject_works_after_idle(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    fake_insert(b, 0);

    b->fdc.phase = FDC_EXEC;
    x68k_bus_eject_fdc(b, 0);
    assert(b->fdc.disk[0] != NULL); /* ビジー中は無視 */

    b->fdc.phase = FDC_IDLE;
    x68k_bus_eject_fdc(b, 0);
    assert(b->fdc.disk[0] == NULL); /* アイドルに戻れば排出される */
    printf("test_button_eject_works_after_idle: OK\n");
}

/* 標準2HD全体分のダミーXDF(全0埋め)を書き出す。x68k_bus_load_fdc()は
 * 実ファイルを要求するため（fake_insert()のようなメモリ直操作は使えない）。 */
static void write_dummy_xdf(const char *path) {
    /* 前回実行がWP系assert失敗でabort()した場合、後始末（末尾のchmod+remove）を
     * 経由せず読み取り専用の一時ファイルが残ることがある（"wb"でのfopen自体が
     * 失敗し道連れで落ちる）。書き込み前に無条件でchmod+removeし、
     * 失敗（未作成でENOENT等）は無視して自己修復する。 */
    chmod(path, 0644);
    remove(path);
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    uint8_t zero[1024] = {0};
    for (int i = 0; i < 1232; i++) {
        size_t written = fwrite(zero, 1, sizeof(zero), fp);
        assert(written == sizeof(zero));
    }
    fclose(fp);
}

/* 7. DDAM状態は永続化しない。0x09(WRITE DELETED DATA)相当で
 *    ddamビットを立てた後に実イジェクト→x68k_bus_insert_fdc_runtime()で
 *    再挿入すると、挿入処理側でもクリアされること
 *    （x68k_bus_eject_fdc()経由の排出クリアとは別経路の回帰確認）。 */
static void test_eject_reinsert_clears_ddam(void) {
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "fdc_eject_test_ddam.XDF");
    write_dummy_xdf(path);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_insert_fdc_runtime(b, 0, path) == 0);

    b->fdc.ddam[0][0] |= 0x01u; /* WRITE DELETED DATA相当でビットを立てる */
    assert(b->fdc.ddam[0][0] != 0);

    x68k_bus_eject_fdc(b, 0);
    assert(b->fdc.disk[0] == NULL);
    assert(b->fdc.ddam[0][0] == 0); /* 排出時にクリアされている */

    /* 排出済みなので今度は既に0のビットマップへ再度ダーティを注入し、
     * 挿入処理側のクリアも独立して働くことを確認する。 */
    b->fdc.ddam[0][0] |= 0x01u;
    assert(x68k_bus_insert_fdc_runtime(b, 0, path) == 0);
    assert(b->fdc.ddam[0][0] == 0); /* 挿入処理(x68k_bus_load_fdc)側でもクリアされる */
    remove(path);
    printf("test_eject_reinsert_clears_ddam: OK\n");
}

/* 8. x68k_bus_load_fdc()経由の挿入でも別イメージのDDAM状態を持ち越さない
 *    こと(D2)。前のディスクでビットを立てた状態から同ドライブへ再度
 *    load_fdc()すると（イジェクトを経ない起動時ロード相当）クリアされる。 */
static void test_load_fdc_clears_ddam(void) {
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "fdc_eject_test_ddam2.XDF");
    write_dummy_xdf(path);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_fdc(b, 0, path) == 0);

    b->fdc.ddam[0][0] |= 0x01u;
    assert(b->fdc.ddam[0][0] != 0);

    assert(x68k_bus_load_fdc(b, 0, path) == 0); /* イジェクトを経ない再ロード */
    assert(b->fdc.ddam[0][0] == 0);
    remove(path);
    printf("test_load_fdc_clears_ddam: OK\n");
}

/* T20: 0644のダミーXDFをload_fdc() → write_protected()==0。 */
static void test_t20_wp_clear_on_writable_file(void) {
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "fdc_eject_test_wp20.XDF");
    write_dummy_xdf(path);
    chmod(path, 0644);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_fdc(b, 0, path) == 0);

    assert(x68k_bus_fdc_write_protected(b, 0) == 0);
    remove(path);
    printf("test_t20_wp_clear_on_writable_file: OK\n");
}

/* T21: chmod(path,0444)してからload_fdc() → write_protected()==1。
 * rootは access(W_OK) が0444でも成功するためスキップする。 */
static void test_t21_wp_set_on_readonly_file(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t21_wp_set_on_readonly_file: SKIP (root)\n");
        return;
    }
#endif
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "fdc_eject_test_wp21.XDF");
    write_dummy_xdf(path);
    chmod(path, 0444);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_fdc(b, 0, path) == 0);

    assert(x68k_bus_fdc_write_protected(b, 0) == 1);
    chmod(path, 0644);
    remove(path);
    printf("test_t21_wp_set_on_readonly_file: OK\n");
}

/* T22: T21の状態からx68k_bus_eject_fdc() → フラグが0に戻る(D1のクリア地点)。 */
static void test_t22_wp_cleared_on_eject(void) {
#ifndef _WIN32
    if (geteuid() == 0) {
        printf("test_t22_wp_cleared_on_eject: SKIP (root)\n");
        return;
    }
#endif
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "fdc_eject_test_wp22.XDF");
    write_dummy_xdf(path);
    chmod(path, 0444);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_fdc(b, 0, path) == 0);
    assert(x68k_bus_fdc_write_protected(b, 0) == 1);

    x68k_bus_eject_fdc(b, 0);
    assert(x68k_bus_fdc_write_protected(b, 0) == 0);
    chmod(path, 0644);
    remove(path);
    printf("test_t22_wp_cleared_on_eject: OK\n");
}

/* T23/T24: set_write_protect()での往復がowner writeビットのみ操作し、
 * group/otherを完全に保存すること（回帰確認）。0664から開始する。 */
static void test_t23_t24_set_write_protect_round_trip(void) {
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "fdc_eject_test_wp2324.XDF");
    write_dummy_xdf(path);
    chmod(path, 0664);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_fdc(b, 0, path) == 0);

    assert(x68k_bus_fdc_set_write_protect(b, 0, 1) == 0);
    assert(x68k_bus_fdc_write_protected(b, 0) == 1);
#ifndef _WIN32
    /* owner writeビットのみがclearされ、group/otherは保持されること。WindowsのNTFS
     * にはowner/group/other粒度のパーミッションが無く、win_compatのchmod/statシムは
     * 単一のread-only属性しか表現できないため、この厳密なビット単位の検証はPOSIXの
     * みで行う（Windows側は直前・直後のwrite_protected()往復で十分検証済み、
     * scsi_wp_test.cのtest_t10_t11_set_write_protect_round_tripと同じ扱い）。 */
    struct stat st1;
    assert(stat(path, &st1) == 0);
    assert((st1.st_mode & 07777) == 0464); /* owner writeのみ落ちる */
#endif

    assert(x68k_bus_fdc_set_write_protect(b, 0, 0) == 0);
    assert(x68k_bus_fdc_write_protected(b, 0) == 0);
#ifndef _WIN32
    struct stat st2;
    assert(stat(path, &st2) == 0);
    assert((st2.st_mode & 07777) == 0664); /* 元のパーミッションに完全復元 */
#endif

    chmod(path, 0644);
    remove(path);
    printf("test_t23_t24_set_write_protect_round_trip: OK\n");
}

/* T25: 未挿入ドライブへset_write_protect() → X68K_FDC_ERR_NO_DISK。 */
static void test_t25_set_write_protect_no_disk(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_fdc_set_write_protect(b, 0, 1) == X68K_FDC_ERR_NO_DISK);
    printf("test_t25_set_write_protect_no_disk: OK\n");
}

/* T26: FDC実行中はset_write_protect()が拒否されること(D8-3)。
 * ファイル権限・フラグとも不変であること。 */
static void test_t26_set_write_protect_busy(void) {
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "fdc_eject_test_wp26.XDF");
    write_dummy_xdf(path);
    chmod(path, 0644);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_fdc(b, 0, path) == 0);

    b->fdc.phase = FDC_CMD;
    assert(x68k_bus_fdc_set_write_protect(b, 0, 1) == X68K_FDC_ERR_BUSY);
    assert(x68k_bus_fdc_write_protected(b, 0) == 0);
#ifndef _WIN32
    /* ファイル権限が不変であることの厳密検証。Windows/NTFSでは表現できない
     * 粒度のためPOSIXのみ（test_t23_t24_set_write_protect_round_tripと同じ理由）。 */
    struct stat st;
    assert(stat(path, &st) == 0);
    assert((st.st_mode & 07777) == 0644);
#endif
    b->fdc.phase = FDC_IDLE;

    remove(path);
    printf("test_t26_set_write_protect_busy: OK\n");
}

/* T27: drive範囲外へset_write_protect() → X68K_FDC_ERR_INVALID_DRIVE。 */
static void test_t27_set_write_protect_invalid_drive(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_fdc_set_write_protect(b, 4, 1) == X68K_FDC_ERR_INVALID_DRIVE);
    assert(x68k_bus_fdc_set_write_protect(b, -1, 1) == X68K_FDC_ERR_INVALID_DRIVE);
    printf("test_t27_set_write_protect_invalid_drive: OK\n");
}

/* T30: 同一の0644ダミーXDFをFD0/FD1の両方へload_fdc() →
 * set_write_protect(b,0,1)で両ドライブとも再判定されること
 * （WP迂回バグの回帰確認）。 */
static void test_t30_set_write_protect_syncs_sibling_drives(void) {
    char path_buf[300];
    const char *path = fixture_path(path_buf, sizeof(path_buf), "fdc_eject_test_wp30.XDF");
    write_dummy_xdf(path);
    chmod(path, 0644);
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_fdc(b, 0, path) == 0);
    assert(x68k_bus_load_fdc(b, 1, path) == 0); /* 同一ファイルをFD1にも挿入 */
    assert(x68k_bus_fdc_write_protected(b, 0) == 0);
    assert(x68k_bus_fdc_write_protected(b, 1) == 0);

    assert(x68k_bus_fdc_set_write_protect(b, 0, 1) == 0);
    assert(x68k_bus_fdc_write_protected(b, 0) == 1);
    assert(x68k_bus_fdc_write_protected(b, 1) == 1); /* FD1も再判定され迂回できない */

    /* FD1へのWRITE DATA相当がNWで拒否され、メモリ・ファイルとも不変であること。 */
    uint8_t before[1024];
    memcpy(before, b->fdc.disk[1], 1024);
    uint8_t ram[16] = {0};
    uint8_t regs[32] = {0};
    b->fdc.bus = b;
    fdc_write_data(&b->fdc, ram, regs, 0x45u); /* MFM|WRITE DATA */
    fdc_write_data(&b->fdc, ram, regs, 0x01u); /* unit1, hd0 */
    fdc_write_data(&b->fdc, ram, regs, 0x00u); /* cyl0 */
    fdc_write_data(&b->fdc, ram, regs, 0x00u); /* head0 */
    fdc_write_data(&b->fdc, ram, regs, 0x01u); /* sec1 */
    fdc_write_data(&b->fdc, ram, regs, 0x03u); /* N=3 */
    fdc_write_data(&b->fdc, ram, regs, 0x01u); /* EOT */
    fdc_write_data(&b->fdc, ram, regs, 0x35u); /* GPL */
    fdc_write_data(&b->fdc, ram, regs, 0xFFu); /* DTL */
    assert(b->fdc.res[0] == (uint8_t)(0x40u | 1u));
    assert(b->fdc.res[1] == 0x02u); /* NW */
    assert(memcmp(before, b->fdc.disk[1], 1024) == 0);

    chmod(path, 0644);
    remove(path);
    printf("test_t30_set_write_protect_syncs_sibling_drives: OK\n");
}

/* 指定バイト数の0埋めファイルを書き出す（サイズ照合テスト用）。 */
static void write_sized_file(const char *path, size_t bytes) {
    remove(path);
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    uint8_t zero[1024] = {0};
    for (size_t left = bytes; left > 0;) {
        size_t n = left < sizeof(zero) ? left : sizeof(zero);
        assert(fwrite(zero, 1, n, fp) == n);
        left -= n;
    }
    fclose(fp);
}

/* 拡張子は大小文字を区別せず標準2HDに対応づけ、正確なサイズだけを
 * 受理する。不一致・未知の拡張子・存在しないファイルは拒否し、既に挿入されている
 * 媒体の状態（イメージ・パス・DDAM・WP・プロファイル）を一切変えない。 */
static void test_load_fdc_validates_extension_and_size(void) {
    char good_buf[300], bad_buf[300], ext_buf[300];
    const char *good = fixture_path(good_buf, sizeof(good_buf), "fdc_mount_good.HdM");
    const char *bad = fixture_path(bad_buf, sizeof(bad_buf), "fdc_mount_bad.xdf");
    const char *ext = fixture_path(ext_buf, sizeof(ext_buf), "fdc_mount_bad.img");
    write_sized_file(good, 1261568u);
    write_sized_file(bad, 1261568u + 1u);
    write_sized_file(ext, 1261568u);

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_fdc(b, 0, good) == 0); /* 大小文字混在の拡張子 */
    assert(b->fdc.profile[0] == &fdc_profile_2hd);

    const uint8_t *img = b->fdc.disk[0];
    int bytes = b->fdc.disk_bytes[0];
    b->fdc.ddam[0][0] = 0x01u;
    b->fdc.write_protect[0] = 1;
    char *path_before = strdup(x68k_bus_fdc_disk_path(b, 0));

    assert(x68k_bus_load_fdc(b, 0, bad) == X68K_FDC_ERR_SIZE_MISMATCH);
    assert(x68k_bus_load_fdc(b, 0, ext) == X68K_FDC_ERR_BAD_EXTENSION);
    assert(x68k_bus_load_fdc(b, 0, "/nonexistent/fdc_mount.xdf") == X68K_FDC_ERR_IO);
    assert(x68k_bus_load_fdc(b, 4, good) == X68K_FDC_ERR_INVALID_DRIVE);
    assert(b->fdc.disk[0] == img && b->fdc.disk_bytes[0] == bytes);
    assert(b->fdc.profile[0] == &fdc_profile_2hd);
    assert(b->fdc.ddam[0][0] == 0x01u && b->fdc.write_protect[0] == 1);
    assert(strcmp(x68k_bus_fdc_disk_path(b, 0), path_before) == 0);
    free(path_before);

    /* 空ドライブへの失敗は空のまま */
    assert(x68k_bus_load_fdc(b, 1, bad) == X68K_FDC_ERR_SIZE_MISMATCH);
    assert(!x68k_bus_fdc_disk_loaded(b, 1) && b->fdc.profile[1] == NULL);

    /* 排出でプロファイルも外れる */
    x68k_bus_eject_fdc(b, 0);
    assert(!x68k_bus_fdc_disk_loaded(b, 0) && b->fdc.profile[0] == NULL);
    remove(good);
    remove(bad);
    remove(ext);
    printf("test_load_fdc_validates_extension_and_size: OK\n");
}

/* 9SCSET系の拡張子は、同サイズでも拡張子の媒体に対応づけて受理し、
 * 別媒体のサイズは拒否する。標準2HD以外へのIOCS _B_FORMAT直接経路は書き込まない。 */
static void test_load_fdc_9sc_extensions(void) {
    static const struct {
        const char *name;
        size_t bytes;
        const fdc_media_profile_t *profile;
    } cases[] = {
        {"fdc_mount_9sc.2HS", 1474560u, &fdc_profile_2hs},
        {"fdc_mount_9sc.2hde", 1474560u, &fdc_profile_2hde},
        {"fdc_mount_9sc.2ht", 1638400u, &fdc_profile_2ht},
    };
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buf[300];
        const char *path = fixture_path(buf, sizeof(buf), cases[i].name);
        write_sized_file(path, cases[i].bytes);
        assert(x68k_bus_load_fdc(b, 0, path) == 0);
        assert(b->fdc.profile[0] == cases[i].profile);
        assert(b->fdc.disk_bytes[0] == (int)cases[i].bytes);
        x68k_bus_eject_fdc(b, 0);
        write_sized_file(path, 1261568u); /* 標準2HDのサイズは拒否 */
        assert(x68k_bus_load_fdc(b, 0, path) == X68K_FDC_ERR_SIZE_MISMATCH);
        remove(path);
    }
    printf("test_load_fdc_9sc_extensions: OK\n");
}

int main(void) {
    test_masked_button_eject_is_ignored();
    test_unmasked_button_eject_works();
    test_register_eject_bypasses_mask();
    test_mask_is_per_drive();
    test_busy_button_eject_is_ignored();
    test_button_eject_works_after_idle();
    test_eject_reinsert_clears_ddam();
    test_load_fdc_clears_ddam();
    test_load_fdc_validates_extension_and_size();
    test_load_fdc_9sc_extensions();
    test_t20_wp_clear_on_writable_file();
    test_t21_wp_set_on_readonly_file();
    test_t22_wp_cleared_on_eject();
    test_t23_t24_set_write_protect_round_trip();
    test_t25_set_write_protect_no_disk();
    test_t26_set_write_protect_busy();
    test_t27_set_write_protect_invalid_drive();
    test_t30_set_write_protect_syncs_sibling_drives();
    printf("fdc_eject_test: all tests passed\n");
    return 0;
}
