/*
 * tools-host/cgrom_gen（BDFから互換CGROMを生成するツール）の単体テスト。
 * 合成した小さなBDFを一時ファイルに書き、配置オフセット・ビットパッキング・
 * BBXの置き直し・文字コードの扱い・エラー検出を検証する。フォントは使わない。
 */
#define CGROM_GEN_NO_MAIN
#include "../tools-host/cgrom_gen/cgrom_gen.c"

#include <assert.h>

#define TMP_BDF_A "cgrom_gen_test_a.bdf"
#define TMP_BDF_B "cgrom_gen_test_b.bdf"
#define TMP_OUT "cgrom_gen_test_out.dat"

#define GLYPH_TEXT_SIZE 4096

static void write_text(const char *path, const char *text) {
    /* assertの中に副作用を置かない（NDEBUGでassertが消えてもファイルは書く）。 */
    FILE *fp = fopen(path, "w");
    int failed = fp == NULL;
    if (!failed)
        failed = fputs(text, fp) < 0;
    if (fp != NULL)
        failed |= fclose(fp) != 0;
    assert(!failed);
}

/* BDFの先頭部分。registryがNULLでなければCHARSET_REGISTRYを持つ。 */
static void append_header(char *out, int w, int h, int xoff, int yoff, const char *registry) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "STARTFONT 2.1\nFONT test\nSIZE %d 75 75\nFONTBOUNDINGBOX %d %d %d %d\n", h, w, h,
             xoff, yoff);
    strcat(out, tmp);
    if (registry != NULL) {
        snprintf(tmp, sizeof tmp, "STARTPROPERTIES 1\nCHARSET_REGISTRY \"%s\"\nENDPROPERTIES\n", registry);
        strcat(out, tmp);
    }
}

/* rowsは各行の16進文字列（BBXの高さ分）を'/'で区切った文字列。 */
static void append_glyph(char *out, int enc, int bw, int bh, int bx, int by, const char *rows) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "STARTCHAR g%d\nENCODING %d\nSWIDTH 500 0\nDWIDTH %d 0\nBBX %d %d %d %d\nBITMAP\n", enc,
             enc, bw, bw, bh, bx, by);
    strcat(out, tmp);
    char copy[256];
    snprintf(copy, sizeof copy, "%s", rows);
    for (char *tok = strtok(copy, "/"); tok != NULL; tok = strtok(NULL, "/")) {
        strcat(out, tok);
        strcat(out, "\n");
    }
    strcat(out, "ENDCHAR\n");
}

static void append_footer(char *out) { strcat(out, "ENDFONT\n"); }

/* 16行ぶん、先頭行だけhex_first、残りは0000の16x16グリフの行指定を作る。 */
static void rows16(char *out, const char *first) {
    strcpy(out, first);
    for (int i = 1; i < 16; i++)
        strcat(out, "/0000");
}

static uint8_t *new_buffer(void) {
    uint8_t *buf = calloc(CGROM_TOTAL_SIZE, 1);
    assert(buf != NULL);
    return buf;
}

static void test_kanji_offsets_and_override(void) {
    char text[GLYPH_TEXT_SIZE] = "";
    char rows[256];
    append_header(text, 16, 16, 0, -2, NULL);
    rows16(rows, "8001");
    append_glyph(text, 0x2121, 16, 16, 0, -2, rows); /* 非漢字の先頭 */
    rows16(rows, "FFFF");
    append_glyph(text, 0x3021, 16, 16, 0, -2, rows); /* 第一水準の先頭 */
    rows16(rows, "1234");
    append_glyph(text, 0x5021, 16, 16, 0, -2, rows); /* 第二水準の先頭 */
    rows16(rows, "ABCD");
    append_glyph(text, 0x747E, 16, 16, 0, -2, rows); /* 第二水準の最後 */
    rows16(rows, "0001");
    append_glyph(text, 0xB0A1, 16, 16, 0, -2, rows); /* EUC形式の0x3021。後勝ちで上書きする */
    rows16(rows, "5555");
    append_glyph(text, 0x2921, 16, 16, 0, -2, rows); /* 区の隙間。対象外 */
    append_footer(text);
    write_text(TMP_BDF_A, text);

    uint8_t *buf = new_buffer();
    load_stats st = {0};
    assert(load_bdf(TMP_BDF_A, SLOT_K16, buf, &st) == 0);
    assert(st.placed == 5 && st.skipped == 1);

    assert(buf[0x000000] == 0x80 && buf[0x000001] == 0x01);
    assert(buf[0x000002] == 0x00); /* 2行目は空 */
    assert(buf[0x005E00] == 0x00 && buf[0x005E01] == 0x01); /* EUC版0001が上書き */
    assert(buf[0x01D600] == 0x12 && buf[0x01D601] == 0x34);
    /* 第二水準の最後: (0x74-0x50)*94 + (0x7E-0x21) = 3477文字目 */
    uint32_t last = 0x01D600u + 3477u * 32u;
    assert(last == 0x0388A0u);
    assert(buf[last] == 0xAB && buf[last + 1] == 0xCD);
    free(buf);
}

static void test_bbx_offset_placement(void) {
    char text[GLYPH_TEXT_SIZE] = "";
    append_header(text, 16, 16, 0, -2, NULL);
    /* 8x8のグリフを(4,0)に置く。セルの上端からの行は 16 + (-2) - (0 + 8) = 6 行目から。 */
    append_glyph(text, 0x2121, 8, 8, 4, 0, "FF/FF/FF/FF/FF/FF/FF/FF");
    append_footer(text);
    write_text(TMP_BDF_A, text);

    uint8_t *buf = new_buffer();
    load_stats st = {0};
    assert(load_bdf(TMP_BDF_A, SLOT_K16, buf, &st) == 0);
    for (int y = 0; y < 16; y++) {
        int inside = y >= 6 && y < 14;
        assert(buf[y * 2] == (inside ? 0x0F : 0x00));     /* 列4〜7 */
        assert(buf[y * 2 + 1] == (inside ? 0xF0 : 0x00)); /* 列8〜11 */
    }
    free(buf);
}

static void test_ank_ranges_and_registry(void) {
    char text[GLYPH_TEXT_SIZE] = "";
    append_header(text, 8, 16, 0, -2, "ISO8859");
    append_glyph(text, 0x41, 8, 16, 0, -2, "11/11/11/11/11/11/11/11/11/11/11/11/11/11/11/11");
    append_glyph(text, 0xB1, 8, 16, 0, -2, "22/22/22/22/22/22/22/22/22/22/22/22/22/22/22/22");
    append_glyph(text, 0x05, 8, 16, 0, -2, "33/33/33/33/33/33/33/33/33/33/33/33/33/33/33/33");
    append_footer(text);
    write_text(TMP_BDF_A, text);

    char text2[GLYPH_TEXT_SIZE] = "";
    append_header(text2, 8, 16, 0, -2, "JISX0201.1976");
    append_glyph(text2, 0xB1, 8, 16, 0, -2, "44/44/44/44/44/44/44/44/44/44/44/44/44/44/44/44");
    append_glyph(text2, 0xA0, 8, 16, 0, -2, "66/66/66/66/66/66/66/66/66/66/66/66/66/66/66/66"); /* 半角カナの直前 */
    append_glyph(text2, 0xA1, 8, 16, 0, -2, "77/77/77/77/77/77/77/77/77/77/77/77/77/77/77/77"); /* 半角カナの先頭 */
    append_glyph(text2, 0xDF, 8, 16, 0, -2, "88/88/88/88/88/88/88/88/88/88/88/88/88/88/88/88"); /* 半角カナの末尾 */
    append_glyph(text2, 0xE0, 8, 16, 0, -2, "99/99/99/99/99/99/99/99/99/99/99/99/99/99/99/99"); /* 半角カナの直後 */
    append_footer(text2);
    write_text(TMP_BDF_B, text2);

    uint8_t *buf = new_buffer();
    load_stats st = {0};
    assert(load_bdf(TMP_BDF_A, SLOT_A8X16, buf, &st) == 0);
    assert(st.placed == 1 && st.skipped == 2); /* 0x41だけ。0xB1(ISO8859)と0x05は対象外 */
    assert(buf[0x03A800 + 0x41 * 16] == 0x11);
    assert(buf[0x03A800 + 0xB1 * 16] == 0x00);
    assert(buf[0x03A800 + 0x05 * 16] == 0x00);

    st = (load_stats){0};
    assert(load_bdf(TMP_BDF_B, SLOT_A8X16, buf, &st) == 0);
    assert(st.placed == 3 && st.skipped == 2);
    assert(buf[0x03A800 + 0xB1 * 16] == 0x44); /* JIS X 0201なら半角カナを取り込む */
    assert(buf[0x03A800 + 0xA0 * 16] == 0x00 && buf[0x03A800 + 0xE0 * 16] == 0x00);
    assert(buf[0x03A800 + 0xA1 * 16] == 0x77 && buf[0x03A800 + 0xDF * 16] == 0x88);
    free(buf);
}

static void test_half_byte_row_packing(void) {
    char text[GLYPH_TEXT_SIZE] = "";
    append_header(text, 12, 12, 0, -2, NULL);
    append_glyph(text, 0x30, 12, 12, 0, -2, "FFF0/8010/8010/8010/8010/8010/8010/8010/8010/8010/8010/FFF0");
    append_footer(text);
    write_text(TMP_BDF_A, text);

    uint8_t *buf = new_buffer();
    load_stats st = {0};
    assert(load_bdf(TMP_BDF_A, SLOT_A12X12, buf, &st) == 0);
    uint32_t off = 0x03B800u + 0x30u * 24u; /* 1文字24バイト（1行2バイト x 12行） */
    assert(buf[off] == 0xFF && buf[off + 1] == 0xF0);
    assert(buf[off + 2] == 0x80 && buf[off + 3] == 0x10);
    assert(buf[off + 22] == 0xFF && buf[off + 23] == 0xF0);
    free(buf);
}

static void test_errors(void) {
    uint8_t *buf = new_buffer();
    load_stats st = {0};

    /* セルの大きさが違うBDF */
    char text[GLYPH_TEXT_SIZE] = "";
    append_header(text, 8, 16, 0, -2, NULL);
    append_footer(text);
    write_text(TMP_BDF_A, text);
    assert(load_bdf(TMP_BDF_A, SLOT_K16, buf, &st) != 0);

    /* 存在しないファイル */
    assert(load_bdf("cgrom_gen_test_missing.bdf", SLOT_K16, buf, &st) != 0);

    /* BDFではないファイル */
    write_text(TMP_BDF_A, "hello\n");
    assert(load_bdf(TMP_BDF_A, SLOT_K16, buf, &st) != 0);

    /* 16進数でないビットマップ行 */
    text[0] = '\0';
    append_header(text, 8, 16, 0, -2, NULL);
    append_glyph(text, 0x41, 8, 2, 0, -2, "ZZ/00");
    append_footer(text);
    write_text(TMP_BDF_A, text);
    assert(load_bdf(TMP_BDF_A, SLOT_A8X16, buf, &st) != 0);

    /* ビットマップの行数がBBXと合わない */
    text[0] = '\0';
    append_header(text, 8, 16, 0, -2, NULL);
    append_glyph(text, 0x41, 8, 3, 0, -2, "00/00");
    append_footer(text);
    write_text(TMP_BDF_A, text);
    assert(load_bdf(TMP_BDF_A, SLOT_A8X16, buf, &st) != 0);
    free(buf);
}

static void test_cli(void) {
    char text[GLYPH_TEXT_SIZE] = "";
    append_header(text, 8, 16, 0, -2, NULL);
    append_glyph(text, 0x41, 8, 16, 0, -2, "18/24/24/42/42/7E/42/42/00/00/00/00/00/00/00/00");
    append_footer(text);
    write_text(TMP_BDF_A, text);

    char *ok[] = {"cgrom_gen", "--a8x16", TMP_BDF_A, "--output", TMP_OUT};
    assert(cgrom_gen_main(5, ok) == 0);
    FILE *fp = fopen(TMP_OUT, "rb");
    assert(fp != NULL);
    uint8_t *data = malloc(CGROM_TOTAL_SIZE + 1);
    assert(data != NULL);
    size_t n = fread(data, 1, CGROM_TOTAL_SIZE + 1, fp);
    fclose(fp);
    assert(n == CGROM_TOTAL_SIZE);
    assert(data[0x03A800 + 0x41 * 16] == 0x18);
    assert(data[0x03A800 + 0x41 * 16 + 5] == 0x7E);
    free(data);

    char *version[] = {"cgrom_gen", "--version"};
    assert(cgrom_gen_main(2, version) == 0);
    char *no_args[] = {"cgrom_gen"};
    assert(cgrom_gen_main(1, no_args) == 2);
    char *no_font[] = {"cgrom_gen", "--output", TMP_OUT};
    assert(cgrom_gen_main(3, no_font) == 2);
    char *unknown[] = {"cgrom_gen", "--bogus", "x"};
    assert(cgrom_gen_main(3, unknown) == 2);
    char *no_value[] = {"cgrom_gen", "--a8x16"};
    assert(cgrom_gen_main(2, no_value) == 2);
    char *bad_font[] = {"cgrom_gen", "--k16", TMP_BDF_A, "--output", TMP_OUT};
    assert(cgrom_gen_main(5, bad_font) == 1); /* 8x16のBDFを16x16に指定 */
}

int main(void) {
    test_kanji_offsets_and_override();
    test_bbx_offset_placement();
    test_ank_ranges_and_registry();
    test_half_byte_row_packing();
    test_errors();
    test_cli();
    remove(TMP_BDF_A);
    remove(TMP_BDF_B);
    remove(TMP_OUT);
    printf("cgrom_gen_test: OK\n");
    return 0;
}
