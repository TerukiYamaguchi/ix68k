/* tools-host/dev/tvram_dump.c
 * T-VRAM ANK フォントを PNG に書き出して目視確認するツール。
 *   -cgrom   <CGROM.DAT>  CGROM ファイル（必須）
 *   -out     <file.png>   ネイティブ解像度 PNG 出力先（省略時: tvram_native.png）
 *   -par-out <file.png>   PAR 補正後 PNG 出力先（省略時: tvram_par.png）
 *
 * X68000 の 512×512 / 256×256 モードはドットが 4:3（横長）なので、
 * 正しく見るには水平方向に 4/3 倍拡大が必要。
 * PAR 補正後 PNG はニアレストネイバーで ×2 水平 ×1.5 垂直にスケールした画像。
 */
#include "bus/x68k_bus.h"
#include "render/renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- 最小限 PNG ライター ---- */

static void pu32be(uint8_t *b, uint32_t v) {
    b[0] = (v >> 24) & 0xFF;
    b[1] = (v >> 16) & 0xFF;
    b[2] = (v >> 8) & 0xFF;
    b[3] = v & 0xFF;
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t buf[4];
    pu32be(buf, len);
    fwrite(buf, 1, 4, f);
    fwrite(type, 1, 4, f);
    uLong crc = crc32(crc32(0L, NULL, 0), (const Bytef *)type, 4);
    if (len > 0 && data) {
        crc = crc32(crc, data, len);
        fwrite(data, 1, len, f);
    }
    pu32be(buf, (uint32_t)crc);
    fwrite(buf, 1, 4, f);
}

/* ARGB8888 フレームバッファを RGB PNG として保存。
 * out_h < 0 のとき h をそのまま使う。 */
static int save_png(const char *path, const uint32_t *argb, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    static const uint8_t sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    pu32be(ihdr + 0, (uint32_t)w);
    pu32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    write_chunk(f, "IHDR", ihdr, 13);

    /* フィルタバイト(0) + RGB の生データ */
    size_t row_bytes = 1u + (size_t)w * 3u;
    size_t raw_len = row_bytes * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) {
        fclose(f);
        return -1;
    }

    for (int y = 0; y < h; y++) {
        raw[y * row_bytes] = 0; /* filter: None */
        for (int x = 0; x < w; x++) {
            uint32_t c = argb[y * w + x];
            /* ARGB8888: bits 23-16=R, 15-8=G, 7-0=B */
            raw[y * row_bytes + 1 + x * 3 + 0] = (c >> 16) & 0xFF;
            raw[y * row_bytes + 1 + x * 3 + 1] = (c >> 8) & 0xFF;
            raw[y * row_bytes + 1 + x * 3 + 2] = c & 0xFF;
        }
    }

    uLongf comp_len = compressBound((uLong)raw_len);
    uint8_t *comp = malloc(comp_len);
    if (!comp) {
        free(raw);
        fclose(f);
        return -1;
    }
    compress2(comp, &comp_len, raw, (uLong)raw_len, Z_BEST_SPEED);
    free(raw);

    write_chunk(f, "IDAT", comp, (uint32_t)comp_len);
    free(comp);
    write_chunk(f, "IEND", NULL, 0);

    fclose(f);
    return 0;
}

/*
 * ニアレストネイバー拡縮。save_png と同じ ARGB8888 形式。
 * src のストライドは src_w、dst は malloc して save_png に渡す。
 */
static int save_png_scaled(const char *path, const uint32_t *src, int src_w, int src_h,
                           int dst_w, int dst_h) {
    uint32_t *dst = malloc((size_t)dst_w * (size_t)dst_h * sizeof(uint32_t));
    if (!dst)
        return -1;
    for (int y = 0; y < dst_h; y++) {
        int sy = y * src_h / dst_h;
        for (int x = 0; x < dst_w; x++) {
            int sx = x * src_w / dst_w;
            dst[y * dst_w + x] = src[sy * src_w + sx];
        }
    }
    int r = save_png(path, dst, dst_w, dst_h);
    free(dst);
    return r;
}

/* ---- メイン ---- */

#define FB_W 512
#define FB_H 512

static uint32_t s_fb[FB_W * FB_H]; /* BSS: ゼロ初期化 */

/* CGROMの8x16 ANKグリフをカラー15でTVRAMへ書く。 */
static void draw_ank(x68k_bus_t *bus, uint8_t ch, int col, int row) {
    const uint8_t *ank = x68k_bus_get_cgrom_ank(bus);
    const uint8_t *font = ank + (((uint32_t)ch + 0x80u) & 0xFFu) * 16u;
    for (int y = 0; y < 16; y++) {
        uint32_t off = (uint32_t)(row * 16 + y) * 128u + (uint32_t)col;
        for (int plane = 0; plane < 4; plane++)
            x68k_bus_write8(bus, 0xE00000u + (uint32_t)plane * 0x20000u + off, font[y]);
    }
}

int main(int argc, char *argv[]) {
    const char *cgrom_path = NULL;
    const char *out_path = "tvram_native.png";
    const char *par_path = "tvram_par.png";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-cgrom") == 0 && i + 1 < argc)
            cgrom_path = argv[++i];
        if (strcmp(argv[i], "-out") == 0 && i + 1 < argc)
            out_path = argv[++i];
        if (strcmp(argv[i], "-par-out") == 0 && i + 1 < argc)
            par_path = argv[++i];
    }
    if (!cgrom_path) {
        fprintf(stderr, "usage: %s -cgrom <CGROM.DAT> [-out native.png] [-par-out par.png]\n",
                argv[0]);
        return 1;
    }

    x68k_bus_t *bus = x68k_bus_create();
    if (!bus) {
        fprintf(stderr, "bus create failed\n");
        return 1;
    }

    if (x68k_bus_load_cgrom(bus, cgrom_path) < 0) {
        fprintf(stderr, "CGROM load failed: %s\n", cgrom_path);
        x68k_bus_destroy(bus);
        return 1;
    }

    /* 全印字 ASCII (0x20–0x7E) を 3 行に分けて描画 */
    for (uint8_t c = 0x20; c <= 0x3F; c++)
        draw_ank(bus, c, c - 0x20, 0);
    for (uint8_t c = 0x40; c <= 0x5F; c++)
        draw_ank(bus, c, c - 0x40, 1);
    for (uint8_t c = 0x60; c <= 0x7E; c++)
        draw_ank(bus, c, c - 0x60, 2);

    /* T-VRAM → フレームバッファ（デフォルトパレット使用）*/
    uint16_t text_pal[16];
    x68k_bus_get_text_palette(bus, text_pal);
    x68k_render_tvram(x68k_bus_get_tvram(bus), s_fb, FB_W, FB_H, text_pal, 0, 0);

    /* ① ネイティブ解像度 PNG（512×48）*/
    int native_h = 3 * 16; /* 48 px: 3 行分 */
    if (save_png(out_path, s_fb, FB_W, native_h) < 0) {
        fprintf(stderr, "PNG write failed: %s\n", out_path);
        x68k_bus_destroy(bus);
        return 1;
    }
    printf("Native  %dx%d -> %s\n", FB_W, native_h, out_path);

    /*
     * ② PAR 補正 PNG（1024×72）
     *    512 水平ドット数モードはドットが 4:3 → 表示幅 ×2、高さ ×1.5
     *    各フォントセル: ネイティブ 8×16px → 表示 16×24px
     *    x2 水平 / x1.5 垂直 = PAR 4/3 ✓
     */
    int par_w = FB_W * 2;         /* 1024 */
    int par_h = native_h * 3 / 2; /* 72  */
    if (save_png_scaled(par_path, s_fb, FB_W, native_h, par_w, par_h) < 0) {
        fprintf(stderr, "PAR PNG write failed: %s\n", par_path);
        x68k_bus_destroy(bus);
        return 1;
    }
    printf("PAR 4:3 %dx%d -> %s\n", par_w, par_h, par_path);

    x68k_bus_destroy(bus);
    return 0;
}
