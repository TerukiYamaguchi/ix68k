/*
 * DMAC(HD63450)のレベル3 IACK が固定値ではなく要求チャネルの NIV を返すことの
 * 単体テスト。bus_iack() は static のため、x68k_bus_get_hooks()
 * が返す iack フックを通して確認する。
 */
#include "bus/bus_internal.h"
#include "mc68000/cpu.h"
#include <assert.h>
#include <stdio.h>

#define DMAC_CH2_NIV 0xE840A5u
#define DMAC_CH3_NIV 0xE840E5u

static x68k_bus_t *new_bus(mc68k_state_t **cpu_out) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create(&hooks);
    assert(cpu != NULL);
    x68k_bus_set_cpu(b, cpu); /* pendingなしのIACKはスプリアス通知を行うため必要 */
    *cpu_out = cpu;
    return b;
}

static void test_live_niv_and_pending_consumption(void) {
    mc68k_state_t *cpu;
    x68k_bus_t *b = new_bus(&cpu);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);

    x68k_bus_write8(b, DMAC_CH2_NIV, 0x70u);
    b->dmac.ch2_irq_pending = 1;
    assert(hooks.iack(b, 3) == 0x70u);
    assert(!b->dmac.ch2_irq_pending);

    x68k_bus_write8(b, DMAC_CH3_NIV, 0x71u);
    b->dmac.ch3_irq_pending = 1;
    assert(hooks.iack(b, 3) == 0x71u);
    assert(!b->dmac.ch3_irq_pending);
    printf("test_live_niv_and_pending_consumption: OK\n");
    mc68k_destroy(cpu);
}

static void test_ch2_priority_and_live_rewrite(void) {
    mc68k_state_t *cpu;
    x68k_bus_t *b = new_bus(&cpu);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);

    x68k_bus_write8(b, DMAC_CH2_NIV, 0x72u);
    x68k_bus_write8(b, DMAC_CH3_NIV, 0x73u);
    b->dmac.ch2_irq_pending = 1;
    b->dmac.ch3_irq_pending = 1;
    assert(hooks.iack(b, 3) == 0x72u);
    assert(!b->dmac.ch2_irq_pending && b->dmac.ch3_irq_pending);
    assert(hooks.iack(b, 3) == 0x73u);

    x68k_bus_write8(b, DMAC_CH2_NIV, 0x74u);
    b->dmac.ch2_irq_pending = 1;
    x68k_bus_write8(b, DMAC_CH2_NIV, 0x75u); /* pending後、IACK前の書き換え */
    assert(hooks.iack(b, 3) == 0x75u);
    printf("test_ch2_priority_and_live_rewrite: OK\n");
    mc68k_destroy(cpu);
}

static void test_reset_niv_and_spurious_iack(void) {
    mc68k_state_t *cpu;
    x68k_bus_t *b = new_bus(&cpu);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);

    x68k_bus_reset_peripheral(b);
    b->dmac.ch2_irq_pending = 1;
    assert(hooks.iack(b, 3) == 0x0Fu); /* $0Fはオートベクタではない */
    assert(!b->dmac.ch2_irq_pending);

    assert(hooks.iack(b, 3) == 0u); /* pendingなしは既存どおりスプリアス通知 */
    printf("test_reset_niv_and_spurious_iack: OK\n");
    mc68k_destroy(cpu);
}

int main(void) {
    test_live_niv_and_pending_consumption();
    test_ch2_priority_and_live_rewrite();
    test_reset_niv_and_spurious_iack();
    printf("dmac_iack_test: all tests passed\n");
    return 0;
}
