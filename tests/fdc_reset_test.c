/*
 * FDC(µPD72065)がRESET命令(x68k_bus_reset_peripheral())および電源投入時
 * (x68k_bus_create())で正しく初期化されることの単体テスト。
 * 回路図でIC203(µPD720651S、40pin DIP)のRESETピン(1番)がRESETネットに直結して
 * いることを確認済み（docs/ix68k-internals/design/fdc_design.md）。従来は
 * bus_reset_peripheral()がb->fdcに一切触れておらず、実行データフェーズ・
 * コマンド/結果バッファ・割り込み要求がソフトリセットをまたいで残留していた。
 * dmac_reset_test.cと同じ流儀（x68k_bus_createはROM不要）。PEDEC側のFDD制御ラッチについても、CPU RESET時の状態を検証する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* fdc_eject_test.cのfake_insert()と同じ流儀: fdc_eject_drive()はdisk[drive]を
 * free()するため、実ファイル不要でheap確保したダミーバッファを直接セットする。 */
static void fake_insert(x68k_bus_t *b, int drive) {
    b->fdc.disk[drive] = malloc(1);
    assert(b->fdc.disk[drive] != NULL);
    b->fdc.disk_bytes[drive] = 1;
}

/* 1. 実行データフェーズ・コマンド/結果バッファ・SCAN比較状態がRESETで
 *    全てクリアされること（本テストの核心）。 */
static void test_exec_phase_cleared(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    uint8_t buf[16] = {0};
    b->fdc.phase = FDC_EXEC;
    b->fdc.cmd_idx = 5;
    b->fdc.cmd_total = 9;
    b->fdc.res_idx = 3;
    b->fdc.res_len = 7;
    b->fdc.dma_ptr = &buf[0];
    b->fdc.dma_remaining = 1024;
    b->fdc.dma_dir = 1;
    b->fdc.dma_drv = 2;
    b->fdc.dma_offset = 512;
    b->fdc.dma_size = 1024;
    b->fdc.dma_cmp = 1;
    b->fdc.scan_mismatch = 1;

    x68k_bus_reset_peripheral(b);

    assert(b->fdc.phase == FDC_IDLE);
    assert(b->fdc.cmd_idx == 0);
    assert(b->fdc.cmd_total == 0);
    assert(b->fdc.res_idx == 0);
    assert(b->fdc.res_len == 0);
    assert(b->fdc.dma_ptr == NULL);
    assert(b->fdc.dma_remaining == 0);
    assert(b->fdc.dma_dir == 0);
    assert(b->fdc.dma_drv == 0);
    assert(b->fdc.dma_offset == 0);
    assert(b->fdc.dma_size == 0);
    assert(b->fdc.dma_cmp == 0);
    assert(b->fdc.scan_mismatch == 0);
    printf("test_exec_phase_cleared: OK\n");
}

/* 2. MSR(CB=bit4)がRESET直後にクリアされること。IPLROMのFDCステータス待ち
 *    ループ(btst #4)がRESET後も抜けられることの回帰ガード（本テストの核心）。 */
static void test_msr_cb_cleared(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    b->fdc.phase = FDC_EXEC;
    x68k_bus_reset_peripheral(b);

    uint8_t msr = x68k_bus_read8(b, 0xE94001u);
    assert((msr & 0x10u) == 0); /* CB=0 */
    assert(msr == 0x80u);       /* RQM=1のみ */
    printf("test_msr_cb_cleared: OK\n");
}

/* 3. メディア挿入状態・ドライブ構成・DDAM・WPはRESETをまたいで保持される
 *    こと（RESET命令は電源断ではない）。 */
static void test_media_state_preserved(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    fake_insert(b, 0);
    b->fdc.num_drives = 4;
    b->fdc.ddam[0][0] = 0xFFu;
    b->fdc.write_protect[0] = 1;

    x68k_bus_reset_peripheral(b);

    assert(b->fdc.disk[0] != NULL);
    assert(b->fdc.disk_bytes[0] == 1);
    assert(b->fdc.num_drives == 4);
    assert(b->fdc.ddam[0][0] == 0xFFu);
    assert(b->fdc.write_protect[0] == 1);
    printf("test_media_state_preserved: OK\n");
}

/* 4. 機械的ヘッド位置(cur_cyl[])はRESETをまたいで保持されること。RESETでは
 *    FLTR/STEP出力がLowになるだけでステップパルスは出ず、ヘッドは動かない
 *    （µPD72065Bデータシート p4脚注）。 */
static void test_cur_cyl_preserved(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    b->fdc.cur_cyl[1] = 40;
    x68k_bus_reset_peripheral(b);
    assert(b->fdc.cur_cyl[1] == 40);
    printf("test_cur_cyl_preserved: OK\n");
}

/* 5. irqがRESETでクリアされ、IOI側(ioi_signal)とも矛盾なく揃うこと。
 *    b->fdc.irqの変化を$E94003書込みハンドラのエッジ検出以外の経路で
 *    IOIへ伝えないため、bus_reset_peripheral()がioi_signal初期化を
 *    fdc_reset()より先に行っていることも合わせて確認する。 */
static void test_irq_cleared_and_ioi_consistent(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    b->fdc.irq = 1;
    b->ioi_signal |= IOI_FDC_SIGNAL;

    x68k_bus_reset_peripheral(b);

    assert(b->fdc.irq == 0);
    assert((b->ioi_signal & IOI_FDC_SIGNAL) == 0);
    printf("test_irq_cleared_and_ioi_consistent: OK\n");
}

/* 6. RESET直後のSENSE INTERRUPT STATUSがST0=$80の1バイトのみを返すこと。
 *    st0を0クリアすると「ドライブ0のシーク正常終了」を偽って名乗ってしまう
 *    （memset不採用の理由）。 */
static void test_sense_interrupt_status_after_reset(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    b->fdc.phase = FDC_EXEC; /* リセット前は実行中だったことにする */
    x68k_bus_reset_peripheral(b);

    x68k_bus_write8(b, 0xE94003u, 0x08u); /* SENSE INTERRUPT STATUS */
    assert(b->fdc.phase == FDC_RESULT);
    assert(b->fdc.res_len == 1);
    uint8_t st0 = x68k_bus_read8(b, 0xE94003u);
    assert(st0 == 0x80u);
    /* 結果フェーズは1バイトで完結し、IDLEへ戻っていること。 */
    assert(b->fdc.phase == FDC_IDLE);
    printf("test_sense_interrupt_status_after_reset: OK\n");
}

/* 7. FDC_EXEC中にRESETされた状態からでも、その後のイジェクトが恒久的に
 *    拒否されないこと。旧実装ではphase != FDC_IDLEのガードにより、
 *    ソフトリセット後もイジェクトが永久に効かなくなっていた
 *    （x68k_bus_eject_fdc()はvoidのため、戻り値ではなくdisk[drive]==NULLで
 *    確認する）。 */
static void test_eject_unblocked_after_reset(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    fake_insert(b, 0);
    b->fdc.phase = FDC_EXEC;
    b->fdc.dma_ptr = b->fdc.disk[0];
    b->fdc.dma_remaining = 1;
    b->fdc.dma_drv = 0;

    x68k_bus_reset_peripheral(b);
    assert(b->fdc.phase == FDC_IDLE); /* ガードが解除されている前提 */

    x68k_bus_eject_fdc(b, 0);
    assert(b->fdc.disk[0] == NULL);
    printf("test_eject_unblocked_after_reset: OK\n");
}

/* 8. 電源投入直後(x68k_bus_create()、resetを挟まない)でもst0=$80であること。
 *    callocのみだとst0=0のままRESET直後と矛盾する応答になる
 *    （必須試験として追加）。 */
static void test_poweron_st0_is_0x80(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE94003u, 0x08u); /* SENSE INTERRUPT STATUS */
    assert(b->fdc.res_len == 1);
    uint8_t st0 = x68k_bus_read8(b, 0xE94003u);
    assert(st0 == 0x80u);
    printf("test_poweron_st0_is_0x80: OK\n");
}

/* 9. $E94005の選択・制御ラッチはRESETでクリアされること。 */
static void test_drive_control_latch_cleared(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    b->drv_ctrl_latch = 0x0Fu;
    x68k_bus_reset_peripheral(b);
    assert(b->drv_ctrl_latch == 0);
    printf("test_drive_control_latch_cleared: OK\n");
}

/* 10. RESET直後のDrive Statusは選択なしとなり0を返す（XEiJと一致）。 */
static void test_drive_status_reads_zero_after_reset(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    fake_insert(b, 0);
    x68k_bus_write8(b, 0xE94005u, 0x01u);
    assert(x68k_bus_read8(b, 0xE94005u) == 0x80u);
    x68k_bus_reset_peripheral(b);
    assert(x68k_bus_read8(b, 0xE94005u) == 0x00u);
    printf("test_drive_status_reads_zero_after_reset: OK\n");
}

/* 11. 物理イジェクトのマスクはRESETをまたいで保持されること。 */
static void test_eject_mask_preserved(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    fake_insert(b, 0);
    x68k_bus_write8(b, 0xE94005u, 0x41u);
    x68k_bus_write8(b, 0xE94005u, 0x40u);
    assert(b->fdc_eject_masked_bits & 0x01u);
    x68k_bus_reset_peripheral(b);
    assert(b->fdc_eject_masked_bits & 0x01u);
    x68k_bus_eject_fdc(b, 0);
    assert(b->fdc.disk[0] != NULL);
    printf("test_eject_mask_preserved: OK\n");
}

/* 12. $E94007由来のアクセスランプ状態はRESETで未選択・motor offへ戻ること。 */
static void test_motor_lamp_state_cleared(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE94007u, 0x81u);
    assert(x68k_bus_fdc_motor_on(b) == 1);
    assert(x68k_bus_fdc_selected_drive(b) == 1);
    x68k_bus_reset_peripheral(b);
    assert(x68k_bus_fdc_motor_on(b) == 0);
    assert(x68k_bus_fdc_selected_drive(b) == 0);
    printf("test_motor_lamp_state_cleared: OK\n");
}

/* 13. 電源投入直後も$E94005の選択ラッチは0であること。 */
static void test_poweron_drive_status_is_zero(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    assert(b->drv_ctrl_latch == 0);
    assert(x68k_bus_read8(b, 0xE94005u) == 0);
    printf("test_poweron_drive_status_is_zero: OK\n");
}

/* 14. RESET前の選択bitを、RESET後の書込みでfall edgeとして扱わないこと。 */
static void test_no_eject_on_stale_edge_after_reset(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    fake_insert(b, 0);
    fake_insert(b, 1);
    x68k_bus_write8(b, 0xE94005u, 0x23u);
    x68k_bus_reset_peripheral(b);
    x68k_bus_write8(b, 0xE94005u, 0x20u);
    assert(b->fdc.disk[0] != NULL);
    assert(b->fdc.disk[1] != NULL);
    printf("test_no_eject_on_stale_edge_after_reset: OK\n");
}

/* 15. 媒体の抜き挿し通知フラグはRESETで失われないこと。 */
static void test_fdc_ejected_flag_preserved(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    fake_insert(b, 0);
    x68k_bus_eject_fdc(b, 0);
    x68k_bus_reset_peripheral(b);
    assert(x68k_bus_fdc_take_ejected_flag(b, 0) == 1);
    printf("test_fdc_ejected_flag_preserved: OK\n");
}

/* 16. RESET後のEJECT MASK解除には、選択bitを立てて落とす2書込みが必要なこと。 */
static void test_eject_mask_needs_two_writes_after_reset(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE94005u, 0x41u);
    x68k_bus_write8(b, 0xE94005u, 0x40u);
    assert(b->fdc_eject_masked_bits & 0x01u);
    x68k_bus_reset_peripheral(b);
    x68k_bus_write8(b, 0xE94005u, 0x01u);
    assert(b->fdc_eject_masked_bits & 0x01u);
    x68k_bus_write8(b, 0xE94005u, 0x00u);
    assert((b->fdc_eject_masked_bits & 0x01u) == 0);
    printf("test_eject_mask_needs_two_writes_after_reset: OK\n");
}

/* 17. RESET処理自身は$E94005の排出副作用を発生させないこと。 */
static void test_reset_itself_does_not_eject(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    fake_insert(b, 0);
    fake_insert(b, 1);
    b->drv_ctrl_latch = 0x2Fu;
    x68k_bus_reset_peripheral(b);
    assert(b->fdc.disk[0] != NULL);
    assert(b->fdc.disk[1] != NULL);
    assert(b->fdc_ejected_flag[0] == 0);
    assert(b->fdc_ejected_flag[1] == 0);
    printf("test_reset_itself_does_not_eject: OK\n");
}

int main(void) {
    test_exec_phase_cleared();
    test_msr_cb_cleared();
    test_media_state_preserved();
    test_cur_cyl_preserved();
    test_irq_cleared_and_ioi_consistent();
    test_sense_interrupt_status_after_reset();
    test_eject_unblocked_after_reset();
    test_poweron_st0_is_0x80();
    test_drive_control_latch_cleared();
    test_drive_status_reads_zero_after_reset();
    test_eject_mask_preserved();
    test_motor_lamp_state_cleared();
    test_poweron_drive_status_is_zero();
    test_no_eject_on_stale_edge_after_reset();
    test_fdc_ejected_flag_preserved();
    test_eject_mask_needs_two_writes_after_reset();
    test_reset_itself_does_not_eject();
    printf("fdc_reset_test: all tests passed\n");
    return 0;
}
