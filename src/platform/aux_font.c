#include "platform/aux_font.h"

int pal_aux_ank_glyph_pixel(const uint8_t *cgrom_ank, uint8_t ch, int col, int row) {
    if (col < 0 || col > 7 || row < 0 || row > 15)
        return 0;
    int idx = (uint8_t)(ch + 0x80u);
    uint8_t byte = cgrom_ank[idx * 16 + row];
    return (byte & (uint8_t)(0x80u >> col)) != 0;
}
