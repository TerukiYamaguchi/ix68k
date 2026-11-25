#include "mcp/screenshot.h"

#include "app_internal.h"
#include "render/renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *mcp_screenshot_make_ppm(x68k_bus_t *bus, int mask, int *out_len) {
    int w = s_app.native_w;
    int h = s_app.native_h;
    char header[64];
    int header_len = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", w, h);
    int body_len = w * h * 3;
    uint32_t *src = s_app.framebuf;
    uint32_t *layer_buf = NULL;

    if (mask != 0) {
        layer_buf = malloc((size_t)w * (size_t)h * sizeof(*layer_buf));
        if (!layer_buf)
            return NULL;
        for (int y = 0; y < h; y++)
            x68k_compose_line_layers(bus, layer_buf, w, h, y, mask);
        src = layer_buf;
    }

    uint8_t *ppm = malloc((size_t)(header_len + body_len));
    if (!ppm) {
        free(layer_buf);
        return NULL;
    }
    memcpy(ppm, header, (size_t)header_len);
    uint8_t *pixel = ppm + header_len;
    for (int i = 0; i < w * h; i++) {
        uint32_t value = src[i];
        pixel[0] = (uint8_t)((value >> 16) & 0xFF);
        pixel[1] = (uint8_t)((value >> 8) & 0xFF);
        pixel[2] = (uint8_t)(value & 0xFF);
        pixel += 3;
    }
    free(layer_buf);
    *out_len = header_len + body_len;
    return ppm;
}
