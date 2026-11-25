/*
 * システムポート#1 ($E8E001 CONTRAST) の時間遷移とLUTの単体テスト。
 * bus/CPU非依存: x68k_bus_t をBSS上に置きcrtc_regsだけ実配列を差す。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <string.h>

static void setup_bus(x68k_bus_t *bus, uint8_t *crtc) {
    memset(bus, 0, sizeof(*bus));
    bus->crtc_regs = crtc;
    bus->crtc.contrast_lut_scaled = -1;
}

static void next_frame(x68k_bus_t *bus) {
    x68k_bus_crtc_raster_frame_start(bus, 10000000 / 60, 512);
}

int main(void) {
    static uint8_t crtc[0x10000];
    static x68k_bus_t bus;

    memset(crtc, 0, sizeof(crtc));
    setup_bus(&bus, crtc);
    crtc[0xE001] = 0x0F;

    /* $E8E001は目標値を直ちに読み返す一方、描画値は次フレームまで変わらない。 */
    assert(x68k_bus_get_contrast(&bus) == 15u);
    crtc[0xE001] = 0x00;
    assert(x68k_bus_get_contrast(&bus) == 0u);
    assert(x68k_bus_get_contrast_scaled(&bus) == 45u);

    /* $F -> $0: 毎フレーム1/45ずつ暗くなり、45フレームで黒へ到達する。 */
    next_frame(&bus);
    assert(x68k_bus_get_contrast_scaled(&bus) == 44u);
    for (int frame = 1; frame < 45; frame++)
        next_frame(&bus);
    assert(x68k_bus_get_contrast_scaled(&bus) == 0u);
    next_frame(&bus);
    assert(x68k_bus_get_contrast_scaled(&bus) == 0u);

    /* $0 -> $F も対称。途中の再書込みは現在値をリセットせず向きを変える。 */
    crtc[0xE001] = 0x0F;
    next_frame(&bus);
    assert(x68k_bus_get_contrast_scaled(&bus) == 1u);
    crtc[0xE001] = 0x00;
    next_frame(&bus);
    assert(x68k_bus_get_contrast_scaled(&bus) == 0u);
    crtc[0xE001] = 0x0F;
    for (int frame = 0; frame < 45; frame++)
        next_frame(&bus);
    assert(x68k_bus_get_contrast_scaled(&bus) == 45u);

    /* scale=45は恒等、scale=0は全黒、scale=24はround(in*24/45)。 */
    const uint8_t *lut = x68k_bus_get_contrast_lut(&bus);
    for (int v = 0; v < 256; v++)
        assert(lut[v] == (uint8_t)v);
    bus.crtc.contrast_attenuation = 45;
    lut = x68k_bus_get_contrast_lut(&bus);
    for (int v = 0; v < 256; v++)
        assert(lut[v] == 0);
    bus.crtc.contrast_attenuation = 21; /* scale = 24 */
    lut = x68k_bus_get_contrast_lut(&bus);
    assert(lut[255] == 136);
    assert(lut[128] == (uint8_t)((128u * 24u + 22u) / 45u));

    /* 16段階の終点は従来のscale=level/15と一致する。上位4bitは無視する。 */
    crtc[0xE001] = 0xF0 | 0x08;
    assert(x68k_bus_get_contrast(&bus) == 8u);
    bus.crtc.contrast_attenuation = 45 - 3 * 8;
    lut = x68k_bus_get_contrast_lut(&bus);
    assert(lut[255] == 136);
    assert(bus.crtc.contrast_lut_scaled == 24);

    return 0;
}
