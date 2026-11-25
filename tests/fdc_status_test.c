/*
 * ディスク選択ウィンドウ向けに追加したFDC状態アクセサの単体
 * テスト: x68k_bus_fdc_disk_loaded()の3/4番ドライブ対応（バグ修正）、
 * x68k_bus_fdc_disk_path()、$E94007由来のx68k_bus_fdc_selected_drive()/
 * x68k_bus_fdc_motor_on()。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fdc_eject_test.cのfake_insert()と同じ流儀: 実ファイル不要でダミー
 * バッファを直接セットして「挿入中」を模擬する。disk_pathも設定する
 * （x68k_bus_load_fdc()を通さないため手動で設定要）。 */
static void fake_insert(x68k_bus_t *b, int drive, const char *path) {
    b->fdc.disk[drive] = malloc(1);
    assert(b->fdc.disk[drive] != NULL);
    b->fdc.disk_bytes[drive] = 1;
    free(b->fdc.disk_path[drive]);
    b->fdc.disk_path[drive] = strdup(path);
}

/* 1. disk_loadedが4ドライブ全てで正しく機能すること（旧実装は
 *    drive>1で常に0を返すバグがあった）。 */
static void test_disk_loaded_all_four_drives(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    x68k_bus_fdc_set_num_drives(b, 4);

    for (int i = 0; i < 4; i++)
        assert(x68k_bus_fdc_disk_loaded(b, i) == 0);

    fake_insert(b, 2, "/tmp/drive2.xdf");
    fake_insert(b, 3, "/tmp/drive3.xdf");
    assert(x68k_bus_fdc_disk_loaded(b, 0) == 0);
    assert(x68k_bus_fdc_disk_loaded(b, 1) == 0);
    assert(x68k_bus_fdc_disk_loaded(b, 2) == 1); /* 旧実装ではここが0の誤りだった */
    assert(x68k_bus_fdc_disk_loaded(b, 3) == 1);

    /* 範囲外は常に0。 */
    assert(x68k_bus_fdc_disk_loaded(b, -1) == 0);
    assert(x68k_bus_fdc_disk_loaded(b, 4) == 0);
    printf("test_disk_loaded_all_four_drives: OK\n");
}

/* 2. disk_pathが挿入中はファイル名を返し、未挿入はNULLを返すこと。 */
static void test_disk_path(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    assert(x68k_bus_fdc_disk_path(b, 0) == NULL);
    fake_insert(b, 0, "/tmp/HUMAN302.XDF");
    assert(x68k_bus_fdc_disk_path(b, 0) != NULL);
    assert(strcmp(x68k_bus_fdc_disk_path(b, 0), "/tmp/HUMAN302.XDF") == 0);
    assert(x68k_bus_fdc_disk_path(b, 1) == NULL); /* 他ドライブは無関係 */
    assert(x68k_bus_fdc_disk_path(b, -1) == NULL);
    assert(x68k_bus_fdc_disk_path(b, 4) == NULL);
    printf("test_disk_path: OK\n");
}

/* 2b. イジェクトするとdisk_pathもNULLに戻ること（ディスク選択ウィンドウで
 *     イジェクト後も古いファイル名が表示され続けていたバグの回帰確認）。 */
static void test_disk_path_cleared_after_eject(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    fake_insert(b, 0, "/tmp/HUMAN302.XDF");
    assert(x68k_bus_fdc_disk_path(b, 0) != NULL);
    x68k_bus_eject_fdc(b, 0);
    assert(x68k_bus_fdc_disk_loaded(b, 0) == 0);
    assert(x68k_bus_fdc_disk_path(b, 0) == NULL);
    printf("test_disk_path_cleared_after_eject: OK\n");
}

/* 3. $E94007書き込みでselected_drive/motor_onが正しく追跡されること
 *    （アクセスランプ用）。 */
static void test_motor_on_tracking(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* 初期状態: モータOFF。 */
    assert(x68k_bus_fdc_motor_on(b) == 0);

    /* bit7=1(MOTOR ON) + bit0-1=10b(drive2)を選択。 */
    x68k_bus_write8(b, 0xE94007u, 0x82u);
    assert(x68k_bus_fdc_selected_drive(b) == 2);
    assert(x68k_bus_fdc_motor_on(b) == 1);

    /* 同じドライブのままbit7=0(MOTOR OFF)に書き換え。 */
    x68k_bus_write8(b, 0xE94007u, 0x02u);
    assert(x68k_bus_fdc_selected_drive(b) == 2);
    assert(x68k_bus_fdc_motor_on(b) == 0);

    /* 別ドライブ(drive0)をモータONで選択し直す。 */
    x68k_bus_write8(b, 0xE94007u, 0x80u);
    assert(x68k_bus_fdc_selected_drive(b) == 0);
    assert(x68k_bus_fdc_motor_on(b) == 1);
    printf("test_motor_on_tracking: OK\n");
}

/* 4. DMAC ch0 CCRへSTART(0x80)を書くとch0_armedが立つこと。 */
static void test_ccr_start_arms_ch0(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(b->dmac.ch0_armed == 0);

    x68k_bus_write8(b, 0xE84007u, 0x80u);
    assert(b->dmac.ch0_armed == 1);
    printf("test_ccr_start_arms_ch0: OK\n");
}

/* 5. CCRへSAB(0x10)を書くとch0_armedが解除されること（本テストの核心）。
 * ROMはFDCがNot Ready等で実行データフェーズに入らずCOCが立たなかったとき、
 * $FF92D8からここへ#$10を書いて転送要求を取り消す。 */
static void test_ccr_sab_disarms_ch0(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE84007u, 0x80u); /* START */
    assert(b->dmac.ch0_armed == 1);
    x68k_bus_write8(b, 0xE84007u, 0x10u); /* SAB */
    assert(b->dmac.ch0_armed == 0);
    printf("test_ccr_sab_disarms_ch0: OK\n");
}

/* 6. SAB後にdmac_ch0_fdc_transfer()を呼んでも古い武装状態を消費して
 * 転送しないこと（新たなSTARTが無い限り次のFDC転送に影響しない）。 */
static void test_sab_then_transfer_is_noop(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE84007u, 0x80u); /* START（武装） */
    x68k_bus_write8(b, 0xE84007u, 0x10u); /* SAB（解除） */
    assert(b->dmac.ch0_armed == 0);
    assert(dmac_ch0_fdc_transfer(b) == 0); /* 消費せず0を返す */
    printf("test_sab_then_transfer_is_noop: OK\n");
}

int main(void) {
    test_disk_loaded_all_four_drives();
    test_disk_path();
    test_disk_path_cleared_after_eject();
    test_motor_on_tracking();
    test_ccr_start_arms_ch0();
    test_ccr_sab_disarms_ch0();
    test_sab_then_transfer_is_noop();
    printf("fdc_status_test: all tests passed\n");
    return 0;
}
