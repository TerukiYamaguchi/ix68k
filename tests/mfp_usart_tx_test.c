/*
 * MFP USART TX（UDR書き込み = ホスト→キーボードのコマンドバイト）の単体テスト
 * （mfp_design.md §5.3）。LED・LED明るさ・リピート開始時間/間隔のデコードと、
 * RX側（UDRラッチ・BF・IPRA bit4・ch_pending）に干渉しないこと、RESET命令を
 * またいでキーボード状態が残ることを確認する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

#define MFP_UDR    0xE8802Fu
#define MFP_RSR    0xE8802Bu
#define MFP_TSR    0xE8802Du
#define TSR_READY  0xFFu
#define RSR_BF_BIT 0x80u
#define IPRA_USART 0x10u

static void tx(x68k_bus_t *b, uint8_t v) {
    mfp_reg_write8(b, MFP_UDR, v);
}

/* 1. LED: $80|~x → 下位7bitが~値。$80=全点灯(0x7F)、$FF=全消灯(0)。 */
static void test_led_decode(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    tx(b, 0x80u);
    assert(x68k_bus_kbd_state(b)->led == 0x7Fu);
    tx(b, 0xFFu);
    assert(x68k_bus_kbd_state(b)->led == 0x00u);
    tx(b, (uint8_t)(0x80u | (uint8_t)~0x08u)); /* CAPSのみ点灯 */
    assert(x68k_bus_kbd_state(b)->led == 0x08u);
    printf("test_led_decode: ok\n");
    x68k_bus_destroy(b);
}

/* 2. リピート開始時間/間隔と境界値（$5F/$60/$6F/$70/$7F）、LED明るさ($54-$57)。 */
static void test_repeat_and_bright_decode(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    const kbd_state_t *k = x68k_bus_kbd_state(b);

    tx(b, 0x5Fu); /* OPT.2 TV制御許可。状態を変えない */
    assert(k->repeat_delay == 0 && k->repeat_rate == 0 && k->led_bright == 0);
    tx(b, 0x60u);
    assert(k->repeat_delay == 0x0);
    tx(b, 0x6Fu);
    assert(k->repeat_delay == 0xF && k->repeat_rate == 0);
    tx(b, 0x70u);
    assert(k->repeat_rate == 0x0);
    tx(b, 0x7Fu);
    assert(k->repeat_rate == 0xF && k->repeat_delay == 0xF);
    tx(b, 0x57u);
    assert(k->led_bright == 0x3);
    tx(b, 0x54u);
    assert(k->led_bright == 0x0);
    assert(k->led == 0); /* 他コマンドでLEDは変わらない */
    printf("test_repeat_and_bright_decode: ok\n");
    x68k_bus_destroy(b);
}

/* 3. 状態を持たないコマンド（$40-$53・$58-$5F）はカウンタのみ進む。 */
static void test_stateless_commands_only_count(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    const uint8_t cmds[] = {0x40, 0x41, 0x49, 0x51, 0x53, 0x58, 0x59, 0x5C, 0x5D};
    for (unsigned i = 0; i < sizeof(cmds); i++)
        tx(b, cmds[i]);
    const kbd_state_t *k = x68k_bus_kbd_state(b);
    assert(k->led == 0 && k->led_bright == 0 && k->repeat_delay == 0 && k->repeat_rate == 0);
    assert(x68k_bus_mfp_usart_tx_count(b) == sizeof(cmds));
    printf("test_stateless_commands_only_count: ok\n");
    x68k_bus_destroy(b);
}

/* 4. TX書き込みはRX側（UDRラッチ・BF・IPRA bit4・ch_pending）に触れない。
 *    TSRは常時readyのまま。 */
static void test_tx_does_not_touch_rx(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    b->mfp.usart_rx_udr = 0x5Au;
    b->mfp.usart_rx_bf = 1;
    b->mfp.ipra |= IPRA_USART;
    mfp_ch_pending_set(&b->mfp, MFP_CH_USART_RX);
    const uint16_t pending = b->mfp.ch_pending;
    const uint8_t ipra = b->mfp.ipra;

    tx(b, 0x80u);
    tx(b, 0x6Au);

    assert(b->mfp.usart_rx_udr == 0x5Au);
    assert(b->mfp.usart_rx_bf == 1);
    assert(b->mfp.ipra == ipra);
    assert(b->mfp.ch_pending == pending);
    assert((mfp_reg_read8(b, MFP_RSR) & RSR_BF_BIT) != 0);
    assert(mfp_reg_read8(b, MFP_TSR) == TSR_READY);
    printf("test_tx_does_not_touch_rx: ok\n");
    x68k_bus_destroy(b);
}

/* 5. キーボード状態はMFPのRESET（RESET命令）で消えない。 */
static void test_state_survives_mfp_reset(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    tx(b, (uint8_t)(0x80u | (uint8_t)~0x20u));
    tx(b, 0x63u);
    tx(b, 0x74u);
    mfp_reset(b);
    const kbd_state_t *k = x68k_bus_kbd_state(b);
    assert(k->led == 0x20u && k->repeat_delay == 3 && k->repeat_rate == 4);
    assert(x68k_bus_mfp_usart_tx_count(b) ==
           3); /* 観測用の累計はusart_rx_droppedと同じくRESETで消さない */
    printf("test_state_survives_mfp_reset: ok\n");
    x68k_bus_destroy(b);
}

int main(void) {
    test_led_decode();
    test_repeat_and_bright_decode();
    test_stateless_commands_only_count();
    test_tx_does_not_touch_rx();
    test_state_survives_mfp_reset();
    printf("mfp_usart_tx_test: all tests passed\n");
    return 0;
}
