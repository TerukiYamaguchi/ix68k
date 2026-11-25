#include "render/video_effects.h"
#include "render/renderer.h"

uint16_t x68k_mix_grb555(uint16_t front, uint16_t back) {
    unsigned g = (((front >> 11) & 0x1Fu) + ((back >> 11) & 0x1Fu)) >> 1;
    unsigned r = (((front >> 6) & 0x1Fu) + ((back >> 6) & 0x1Fu)) >> 1;
    unsigned b = (((front >> 1) & 0x1Fu) + ((back >> 1) & 0x1Fu)) >> 1;
    return (uint16_t)((g << 11) | (r << 6) | (b << 1) | (back & 1u));
}

int x68k_r2_in_region(const x68k_r2_state_t *r2, int is_64k, const uint16_t *pal_raw,
                      uint16_t code) {
    if (r2->lsb_source == X68K_R2_LSB_PAGE)
        return (code & 1u) != 0;
    if (is_64k)
        return (x68k_gfx_pal64k_resolve(pal_raw, (uint16_t)(code | 1u)) & 1u) != 0;
    return (pal_raw[code & (uint16_t)~1u] & 1u) != 0;
}

uint16_t x68k_r2_base_color(const x68k_r2_state_t *r2, int is_64k, const uint16_t *pal_raw,
                            uint16_t code) {
    if (r2->lsb_source == X68K_R2_LSB_PAGE)
        code &= (uint16_t)~1u;
    return is_64k ? x68k_gfx_pal64k_resolve(pal_raw, code) : pal_raw[code];
}
