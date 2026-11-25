/*
 * fdc.c RECALIBRATE の CT2(OPM $1B bit6)プローブモード分岐の単体テスト。
 * bus/CPU非依存: dmac_ch0_fdc_transfer()はREAD/WRITE DATA経路専用でRECALIBRATE/
 * SENSE INTERRUPT STATUSからは呼ばれないため、リンクだけ満たすスタブを与える。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>

int dmac_ch0_fdc_transfer(x68k_bus_t *b) {
    (void)b;
    return 0;
}

static uint8_t st0_via_recalibrate(x68k_bus_t *bus, uint8_t unit) {
    uint8_t ram[16] = {0};
    uint8_t crtc_regs[32] = {0};

    fdc_write_data(&bus->fdc, ram, crtc_regs, 0x07u); /* RECALIBRATE */
    fdc_write_data(&bus->fdc, ram, crtc_regs, unit);

    fdc_write_data(&bus->fdc, ram, crtc_regs, 0x08u); /* SENSE INTERRUPT STATUS */
    uint8_t st0 = fdc_read_data(&bus->fdc);
    fdc_read_data(&bus->fdc); /* PCN(2バイト目)も読み切ってFDC_RESULTを抜ける */
    return st0;
}

int main(void) {
    /* staticでBSSに置く: x68k_bus_tをスタックに直接置くとWindowsの既定スタック
     * サイズ(1MB)を超えてSTATUS_STACK_OVERFLOWになる（実測で判明）。 */
    static x68k_bus_t bus = {0};
    bus.fdc.bus = &bus;
    bus.fdc.num_drives = 2; /* external_fdd=false相当 */

    /* CT2=0（通常動作）: メディア未挿入のdrive0はNot Ready(0x48)を返す（回帰防止） */
    assert(st0_via_recalibrate(&bus, 0) == (uint8_t)(0x48u | 0u));

    /* CT2=1（プローブモード）: drive0/1は物理存在するので成功(EC=0) */
    adpcm_opm_write8(&bus.adpcm, 0x01u, 0x1Bu);
    adpcm_opm_write8(&bus.adpcm, 0x03u, 0x40u); /* CT2=1 */
    assert(st0_via_recalibrate(&bus, 0) == (uint8_t)(0x20u | 0u));
    assert(st0_via_recalibrate(&bus, 1) == (uint8_t)(0x20u | 1u));

    /* CT2=1・drive2/3は num_drives=2 を超えるため物理不在 → EC(bit4)が立つ */
    assert(st0_via_recalibrate(&bus, 2) == (uint8_t)(0x70u | 2u));
    assert(st0_via_recalibrate(&bus, 3) == (uint8_t)(0x70u | 3u));

    /* CT2を戻すとdrive2もメディア未挿入のNot Ready(0x48)に戻る（CT2=0経路は不変） */
    adpcm_opm_write8(&bus.adpcm, 0x01u, 0x1Bu);
    adpcm_opm_write8(&bus.adpcm, 0x03u, 0x00u); /* CT2=0 */
    assert(st0_via_recalibrate(&bus, 2) == (uint8_t)(0x48u | 2u));

    printf("fdc_probe_test: OK\n");
    return 0;
}
