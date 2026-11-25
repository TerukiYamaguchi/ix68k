/*
 * ジョイスティック(PPIポートA/B)の単体テスト。
 * $E9A001/$E9A003の未接続時0xFF・x68k_bus_set_joystick()反映・
 * ポートC($E9A005、ADPCM共用)への波及が無いことを確認する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

/* 1. 未接続時（既定状態）はポートA/Bとも0xFF（プルアップ相当）。 */
static void test_default_unconnected(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    assert(x68k_bus_read8(b, 0xE9A001u) == 0xFFu);
    assert(x68k_bus_read8(b, 0xE9A003u) == 0xFFu);
    printf("test_default_unconnected: OK\n");
}

/* 2. x68k_bus_set_joystick()の値がポートA/Bへそれぞれ独立に反映されること。 */
static void test_set_joystick_reflected(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* 上方向(bit0)+トリガA(bit5)押下相当: 該当ビットを0にする。 */
    x68k_bus_set_joystick(b, 0, (uint8_t)~(0x01u | 0x20u));
    assert(x68k_bus_read8(b, 0xE9A001u) == (uint8_t)~(0x01u | 0x20u));
    assert(x68k_bus_read8(b, 0xE9A003u) == 0xFFu); /* #2は無関係のまま */

    /* #2側も独立に反映される。 */
    x68k_bus_set_joystick(b, 1, (uint8_t)~0x08u); /* 右方向 */
    assert(x68k_bus_read8(b, 0xE9A001u) == (uint8_t)~(0x01u | 0x20u)); /* #1は変わらず */
    assert(x68k_bus_read8(b, 0xE9A003u) == (uint8_t)~0x08u);
    printf("test_set_joystick_reflected: OK\n");
}

/* 3. 範囲外indexは無視される（クラッシュしない）。 */
static void test_set_joystick_out_of_range(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_set_joystick(b, -1, 0x00u);
    x68k_bus_set_joystick(b, 2, 0x00u);
    assert(x68k_bus_read8(b, 0xE9A001u) == 0xFFu);
    assert(x68k_bus_read8(b, 0xE9A003u) == 0xFFu);
    printf("test_set_joystick_out_of_range: OK\n");
}

/* 4. ポートC($E9A005、ADPCMパン/レート共用)はポートA/B追加の影響を受けない
 *    （設計時の懸念事項）。書き込んだ値がそのまま
 *    読み戻せること（adpcm_ppi_read8への委譲が生きていること）を確認。 */
static void test_port_c_unaffected(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE9A005u, 0x55u);
    assert(x68k_bus_read8(b, 0xE9A005u) == 0x55u);
    printf("test_port_c_unaffected: OK\n");
}

/* 5. ポートC bit4/bit5=1で#1/#2の操作状態が通知されなくなる（0xFF）。
 *    0に戻せば元の値に復帰し、片側の禁止はもう片側に影響しない。 */
static void test_joy_disable_by_port_c(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_set_joystick(b, 0, 0xFEu); /* #1 上 */
    x68k_bus_set_joystick(b, 1, 0xFBu); /* #2 左 */

    x68k_bus_write8(b, 0xE9A005u, 0x10u); /* PC4=1 */
    assert(x68k_bus_read8(b, 0xE9A001u) == 0xFFu);
    assert(x68k_bus_read8(b, 0xE9A003u) == 0xFBu);

    x68k_bus_write8(b, 0xE9A005u, 0x20u); /* PC5=1 */
    assert(x68k_bus_read8(b, 0xE9A001u) == 0xFEu);
    assert(x68k_bus_read8(b, 0xE9A003u) == 0xFFu);

    x68k_bus_write8(b, 0xE9A007u, 0x09u); /* ビットセット/リセット: PC4=1 */
    assert(x68k_bus_read8(b, 0xE9A001u) == 0xFFu);

    x68k_bus_write8(b, 0xE9A005u, 0x00u);
    assert(x68k_bus_read8(b, 0xE9A001u) == 0xFEu);
    assert(x68k_bus_read8(b, 0xE9A003u) == 0xFBu);
    printf("test_joy_disable_by_port_c: OK\n");
}

/* 6. ポートC bit6(IOA5)/bit7(IOA6)=1で#1のトリガA/B(bit5/bit6)が0になる。
 *    #2には影響しない。 */
static void test_joy1_trigger_force_by_port_c(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    x68k_bus_write8(b, 0xE9A005u, 0x40u); /* IOA5=1 */
    assert(x68k_bus_read8(b, 0xE9A001u) == 0xDFu);
    assert(x68k_bus_read8(b, 0xE9A003u) == 0xFFu);

    x68k_bus_write8(b, 0xE9A005u, 0x80u); /* IOA6=1 */
    assert(x68k_bus_read8(b, 0xE9A001u) == 0xBFu);
    assert(x68k_bus_read8(b, 0xE9A003u) == 0xFFu);

    x68k_bus_write8(b, 0xE9A005u, 0xC0u);
    assert(x68k_bus_read8(b, 0xE9A001u) == 0x9Fu);

    x68k_bus_set_joystick(b, 0, 0xFEu); /* 上方向は保持される */
    assert(x68k_bus_read8(b, 0xE9A001u) == 0x9Eu);

    x68k_bus_write8(b, 0xE9A005u, 0x00u);
    assert(x68k_bus_read8(b, 0xE9A001u) == 0xFEu);
    printf("test_joy1_trigger_force_by_port_c: OK\n");
}

int main(void) {
    test_default_unconnected();
    test_set_joystick_reflected();
    test_set_joystick_out_of_range();
    test_port_c_unaffected();
    test_joy_disable_by_port_c();
    test_joy1_trigger_force_by_port_c();
    printf("joystick_ppi_test: all tests passed\n");
    return 0;
}
