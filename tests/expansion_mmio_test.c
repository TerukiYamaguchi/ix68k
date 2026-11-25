/*
 * 拡張スロット領域($EA0000-$EAFFFF)の読み出しデコードの単体テスト。
 *
 * HOSTDRV($EA0200-$EA020F)・HOSTFILE($EA0100-$EA0117)の裏口レジスタと
 * RTCテストバイト($EAFFEB)が read8 経路で正しく実体へ届くことを確認する。
 * read8 のディスパッチ条件が個別ウィンドウの定数で組まれていると
 * HOSTFILE_BASE < HOSTDRV_RD_BASE のため空範囲になり、ここでの読み出しが
 * すべて 0xFF へ落ちる（HOSTDRV.SYS がマジック'HD'を読めず組み込みに失敗する）。
 */
#include "bus/x68k_bus.h"
#include <assert.h>
#include <stdio.h>

#define HOSTDRV_MAGIC_ADDR   0xEA0200u
#define HOSTFILE_MAGIC_ADDR 0xEA0100u
#define DEV_RTC_TEST_ADDR   0xEAFFEBu

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* --- A: ホストディレクトリ未設定なら HOSTDRV マジックは 0 --- */
    assert(x68k_bus_read8(b, HOSTDRV_MAGIC_ADDR) == 0x00u);
    assert(x68k_bus_read8(b, HOSTDRV_MAGIC_ADDR + 1) == 0x00u);
    printf("expansion_mmio_test: A (HOSTDRV未設定時のマジック) OK\n");

    /* --- B: 設定後は 'H','D' を返す（HOSTDRV.SYS の初期化が参照する） --- */
    x68k_bus_hostdrv_configure(b, ".");
    assert(x68k_bus_read8(b, HOSTDRV_MAGIC_ADDR) == 0x48u);
    assert(x68k_bus_read8(b, HOSTDRV_MAGIC_ADDR + 1) == 0x44u);
    printf("expansion_mmio_test: B (HOSTDRV設定後のマジック) OK\n");

    /* --- C: HOSTFILE のマジック上位バイトは hostdrv_dir 設定に依存せず 'H' --- */
    assert(x68k_bus_read8(b, HOSTFILE_MAGIC_ADDR) == 0x48u);
    printf("expansion_mmio_test: C (HOSTFILEマジック) OK\n");

    /* --- D: $EAFFEB(RTCテストバイト)は書き込みを保持して読み返す --- */
    x68k_bus_write8(b, DEV_RTC_TEST_ADDR, 0x5Au);
    assert(x68k_bus_read8(b, DEV_RTC_TEST_ADDR) == 0x5Au);
    printf("expansion_mmio_test: D (RTCテストバイト) OK\n");

    /* --- E: 実体のない拡張スロット領域は副作用のない 0x00 を返す --- */
    assert(x68k_bus_read8(b, 0xEA0000u) == 0x00u);
    assert(x68k_bus_read8(b, 0xEA8000u) == 0x00u);
    printf("expansion_mmio_test: E (未実装領域は0x00) OK\n");

    x68k_bus_destroy(b);
    printf("expansion_mmio_test: ALL OK\n");
    return 0;
}
