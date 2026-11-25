/*
 * cgrom_gen - ビットマップフォント(BDF)から互換CGROM(768KB)を生成する。
 *
 * C99 / 依存なし。フォント自体は同梱しない（各自が用意したBDFを指定する）。
 *
 * アドレス配置・ビットパッキングは『インサイドX68000』4章「CGROM」（図39〜図46）から
 * 書き起こした。WinX68k/XM6Util等の既存CGROM合成ツールのソースは参照していない。
 *
 * 入力はBDF（X11のビットマップフォント形式）。日本語BDFのENCODINGはJIS X 0208の
 * 区点コード（0x2121〜）なので、文字コード変換表は要らない。サイズごとにBDFを
 * 指定する。指定しないサイズは空白のまま出力する。
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CGROM_GEN_VERSION "2.0"
#define CGROM_TOTAL_SIZE 0x0C0000u

#define MAX_CELL_SIZE 32     /* セルの幅・高さの上限（行をuint32_tに収める） */
#define MAX_GLYPH_SIZE 64    /* BBXの幅・高さの上限（異常なBDFの拒否用） */
#define MAX_FILES_PER_SLOT 4 /* 1つのサイズに指定できるBDFの数 */
#define LINE_BUF_SIZE 1024

/* JIS X 0208の下位バイト範囲（上位は区ごとに異なる）。1区94文字。 */
#define JIS_LO_FIRST 0x21u
#define JIS_LO_LAST 0x7Eu
#define JIS_ROW_CHARS 94u

/* 半角(ANK)で取り込む範囲: JIS X 0201のラテン部分と半角カタカナ。 */
#define ANK_LATIN_FIRST 0x20
#define ANK_LATIN_LAST 0x7E
#define ANK_KANA_FIRST 0xA1
#define ANK_KANA_LAST 0xDF

/* EUC形式（上位・下位バイトの最上位ビットが立つ）のコードをJISに戻すマスク */
#define EUC_BITS_MASK 0x8080
#define EUC_TO_JIS_MASK 0x7F7F

typedef enum {
    SLOT_K16,
    SLOT_K24,
    SLOT_A8X8,
    SLOT_A8X16,
    SLOT_A12X12,
    SLOT_A12X24,
    SLOT_COUNT
} slot_id;

#define KANJI_PARTS 3

typedef struct {
    const char *option;
    const char *label;
    int width;
    int height;
    int is_kanji;
    /* 漢字: 非漢字・第一水準・第二水準の先頭オフセット。半角: base[0]のみ。 */
    uint32_t base[KANJI_PARTS];
} slot_info;

/* 図40（CGROMアドレス配置）のオフセット。$F00000を0とした相対値。 */
static const slot_info SLOTS[SLOT_COUNT] = {
    [SLOT_K16] = {"--k16", "16x16漢字", 16, 16, 1, {0x000000u, 0x005E00u, 0x01D600u}},
    [SLOT_K24] = {"--k24", "24x24漢字", 24, 24, 1, {0x040000u, 0x04D380u, 0x082180u}},
    [SLOT_A8X8] = {"--a8x8", "8x8半角", 8, 8, 0, {0x03A000u, 0, 0}},
    [SLOT_A8X16] = {"--a8x16", "8x16半角", 8, 16, 0, {0x03A800u, 0, 0}},
    [SLOT_A12X12] = {"--a12x12", "12x12半角", 12, 12, 0, {0x03B800u, 0, 0}},
    [SLOT_A12X24] = {"--a12x24", "12x24半角", 12, 24, 0, {0x03D000u, 0, 0}},
};

typedef struct {
    unsigned hi_first;
    unsigned hi_last;
} kanji_part;

/* JIS X 0208の上位バイト範囲: 非漢字8区、第一水準32区、第二水準37区。 */
static const kanji_part KANJI_PART_RANGES[KANJI_PARTS] = {
    {0x21u, 0x28u},
    {0x30u, 0x4Fu},
    {0x50u, 0x74u},
};

typedef struct {
    unsigned placed;  /* CGROMへ書いたグリフ数 */
    unsigned skipped; /* 対象外の文字コードで読み飛ばしたグリフ数 */
} load_stats;

static size_t bytes_per_row(int width) { return (size_t)((width + 7) / 8); }

static size_t bytes_per_char(const slot_info *slot) {
    return bytes_per_row(slot->width) * (size_t)slot->height;
}

/*
 * 1文字分のセル（行ごとのビット列。左端がMSB）をCGROMのバイト列に詰める。
 * 1ラインwidthビットを左から詰め、バイト境界まで0でパディングする
 * （12x12/12x24の下位4bitが常に0になるのはこの規則の帰結、図43/44準拠）。
 */
static void pack_cell(const slot_info *slot, const uint32_t *rows, uint8_t *dst) {
    size_t nbytes = bytes_per_row(slot->width);
    int pad = (int)(nbytes * 8) - slot->width;
    for (int y = 0; y < slot->height; y++) {
        uint32_t val = rows[y] << pad;
        for (size_t k = 0; k < nbytes; k++)
            dst[(size_t)y * nbytes + k] = (uint8_t)(val >> (8u * (nbytes - 1u - k)));
    }
}

/* 文字コードに対応するCGROM内のオフセットを求める。対象外なら0を返す。 */
static int glyph_offset(const slot_info *slot, int encoding, int skip_high_ank, uint32_t *out) {
    size_t bpc = bytes_per_char(slot);
    if (slot->is_kanji) {
        unsigned code = (unsigned)encoding;
        if ((code & EUC_BITS_MASK) == EUC_BITS_MASK)
            code &= EUC_TO_JIS_MASK;
        unsigned hi = code >> 8;
        unsigned lo = code & 0xFFu;
        if (encoding < 0 || code > 0xFFFFu || lo < JIS_LO_FIRST || lo > JIS_LO_LAST)
            return 0;
        for (int p = 0; p < KANJI_PARTS; p++) {
            if (hi >= KANJI_PART_RANGES[p].hi_first && hi <= KANJI_PART_RANGES[p].hi_last) {
                unsigned index = (hi - KANJI_PART_RANGES[p].hi_first) * JIS_ROW_CHARS + (lo - JIS_LO_FIRST);
                *out = slot->base[p] + (uint32_t)(index * bpc);
                return 1;
            }
        }
        return 0;
    }
    int is_latin = encoding >= ANK_LATIN_FIRST && encoding <= ANK_LATIN_LAST;
    int is_kana = encoding >= ANK_KANA_FIRST && encoding <= ANK_KANA_LAST && !skip_high_ank;
    if (!is_latin && !is_kana)
        return 0;
    *out = slot->base[0] + (uint32_t)((size_t)encoding * bpc);
    return 1;
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int starts_with(const char *s, const char *prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

static void strip_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = '\0';
}

/* CHARSET_REGISTRY "ISO8859" のようなプロパティ行から、値がISO8859系かを判定する。 */
static int registry_is_iso8859(const char *line) {
    const char *q = strchr(line, '"');
    if (q == NULL)
        return 0;
    q++;
    for (size_t i = 0; i < 7; i++) {
        if (q[i] == '\0' || toupper((unsigned char)q[i]) != "ISO8859"[i])
            return 0;
    }
    return 1;
}

typedef struct {
    int width, height, xoff, yoff; /* FONTBOUNDINGBOX */
    int have_bbox;
    int iso8859; /* ISO8859系のBDFは半角カタカナの位置にラテン1補助文字を持つので取り込まない */
} font_header;

typedef struct {
    int encoding;
    int bw, bh, bx, by; /* BBX */
    int have_bbx;
    int row;
    uint32_t cell[MAX_CELL_SIZE];
} glyph_state;

/* グリフのビットマップ1行（16進文字列）をセルに書き込む。 */
static int apply_bitmap_row(const font_header *fh, glyph_state *g, const char *hex) {
    size_t nbytes = bytes_per_row(g->bw);
    uint8_t bytes[MAX_GLYPH_SIZE / 8 + 1] = {0};
    for (size_t i = 0; i < nbytes; i++) {
        int hi = hex_value((unsigned char)hex[i * 2]);
        int lo = hi < 0 ? -1 : hex_value((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    /* BBXのオフセットから、FONTBOUNDINGBOX（セル）内の座標へ置き直す。 */
    int cy = fh->height + fh->yoff - (g->by + g->bh) + g->row;
    for (int x = 0; x < g->bw; x++) {
        if (!((bytes[x >> 3] >> (7 - (x & 7))) & 1))
            continue;
        int cx = g->bx - fh->xoff + x;
        if (cx >= 0 && cx < fh->width && cy >= 0 && cy < fh->height)
            g->cell[cy] |= 1u << (fh->width - 1 - cx);
    }
    return 1;
}

/* BDF1ファイルを読み、スロットのサイズに合うグリフをbufへ書く。成功で0。 */
static int load_bdf(const char *path, slot_id id, uint8_t *buf, load_stats *stats) {
    const slot_info *slot = &SLOTS[id];
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "cgrom_gen: %s を開けません\n", path);
        return 1;
    }
    font_header fh = {0};
    glyph_state g = {0};
    char line[LINE_BUF_SIZE];
    int in_bitmap = 0;
    int status = 0;

    while (status == 0 && fgets(line, sizeof line, fp) != NULL) {
        strip_newline(line);
        if (in_bitmap) {
            if (strcmp(line, "ENDCHAR") == 0) {
                uint32_t off;
                if (g.row != g.bh) {
                    fprintf(stderr, "cgrom_gen: %s: ビットマップの行数がBBXと合いません\n", path);
                    status = 1;
                } else if (glyph_offset(slot, g.encoding, fh.iso8859, &off)) {
                    pack_cell(slot, g.cell, buf + off);
                    stats->placed++;
                } else {
                    stats->skipped++;
                }
                in_bitmap = 0;
            } else if (g.row < g.bh) {
                if (!apply_bitmap_row(&fh, &g, line)) {
                    fprintf(stderr, "cgrom_gen: %s: ビットマップの行が16進数ではありません\n", path);
                    status = 1;
                }
                g.row++;
            }
        } else if (starts_with(line, "FONTBOUNDINGBOX")) {
            if (sscanf(line, "FONTBOUNDINGBOX %d %d %d %d", &fh.width, &fh.height, &fh.xoff, &fh.yoff) != 4) {
                fprintf(stderr, "cgrom_gen: %s: FONTBOUNDINGBOXを読めません\n", path);
                status = 1;
            } else if (fh.width != slot->width || fh.height != slot->height) {
                fprintf(stderr, "cgrom_gen: %s: セルが%dx%dです。%s（%s）には%dx%dのBDFが要ります\n", path,
                        fh.width, fh.height, slot->option, slot->label, slot->width, slot->height);
                status = 1;
            } else {
                fh.have_bbox = 1;
            }
        } else if (starts_with(line, "CHARSET_REGISTRY")) {
            fh.iso8859 = registry_is_iso8859(line);
        } else if (starts_with(line, "STARTCHAR")) {
            memset(&g, 0, sizeof g);
            g.encoding = -1;
        } else if (starts_with(line, "ENCODING")) {
            if (sscanf(line, "ENCODING %d", &g.encoding) != 1)
                g.encoding = -1;
        } else if (starts_with(line, "BBX")) {
            g.have_bbx = sscanf(line, "BBX %d %d %d %d", &g.bw, &g.bh, &g.bx, &g.by) == 4 && g.bw > 0 &&
                         g.bh > 0 && g.bw <= MAX_GLYPH_SIZE && g.bh <= MAX_GLYPH_SIZE;
            if (!g.have_bbx) {
                fprintf(stderr, "cgrom_gen: %s: BBXが不正です\n", path);
                status = 1;
            }
        } else if (strcmp(line, "BITMAP") == 0) {
            if (!fh.have_bbox || !g.have_bbx) {
                fprintf(stderr, "cgrom_gen: %s: BITMAPの前にFONTBOUNDINGBOX/BBXがありません\n", path);
                status = 1;
            }
            memset(g.cell, 0, sizeof g.cell);
            g.row = 0;
            in_bitmap = 1;
        }
    }
    if (status == 0 && ferror(fp)) {
        fprintf(stderr, "cgrom_gen: %s の読み込みに失敗しました\n", path);
        status = 1;
    }
    if (status == 0 && !fh.have_bbox) {
        fprintf(stderr, "cgrom_gen: %s: BDFとして読めません（FONTBOUNDINGBOXがありません）\n", path);
        status = 1;
    }
    fclose(fp);
    return status;
}

typedef struct {
    const char *files[SLOT_COUNT][MAX_FILES_PER_SLOT];
    int count[SLOT_COUNT];
    const char *output;
    int verbose;
} options;

static void print_usage(FILE *out) {
    fprintf(out,
            "使い方: cgrom_gen --output <CGROM.DAT> [サイズごとのBDF...] [-v]\n"
            "\n"
            "  --k16 <BDF>     16x16漢字（JIS X 0208。1区94文字の区点コード順）\n"
            "  --k24 <BDF>     24x24漢字\n"
            "  --a8x8 <BDF>    8x8半角\n"
            "  --a8x16 <BDF>   8x16半角\n"
            "  --a12x12 <BDF>  12x12半角\n"
            "  --a12x24 <BDF>  12x24半角\n"
            "  --output <path> 出力先（768KB）\n"
            "  -v              サイズごとの取り込み数を表示\n"
            "  --version       バージョンを表示\n"
            "\n"
            "BDFのセルの大きさは、サイズごとの指定と一致させる。半角は同じオプションを\n"
            "繰り返して複数のBDFを重ねられる（後のファイルが前のファイルを上書きする）。\n"
            "指定しないサイズは空白のまま出力する。\n");
}

/* 引数を解釈する。続行できるときは0、すぐ終了すべきとき（--version、--help、エラー）は1を返し、
 * その終了コードを*exit_codeへ入れる。 */
static int parse_args(int argc, char **argv, options *opt, int *exit_code) {
    *exit_code = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--version") == 0) {
            printf("cgrom_gen %s\n", CGROM_GEN_VERSION);
            return 1;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout);
            return 1;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            opt->verbose = 1;
            continue;
        }
        int is_slot = 0;
        int is_output = strcmp(arg, "--output") == 0;
        for (int s = 0; s < SLOT_COUNT; s++)
            if (strcmp(arg, SLOTS[s].option) == 0)
                is_slot = 1;
        if (!is_slot && !is_output) {
            fprintf(stderr, "cgrom_gen: 不明なオプション: %s\n", arg);
            print_usage(stderr);
            *exit_code = 2;
            return 1;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "cgrom_gen: %s に値がありません\n", arg);
            *exit_code = 2;
            return 1;
        }
        const char *value = argv[++i];
        if (is_output) {
            opt->output = value;
            continue;
        }
        for (int s = 0; s < SLOT_COUNT; s++) {
            if (strcmp(arg, SLOTS[s].option) != 0)
                continue;
            if (opt->count[s] >= MAX_FILES_PER_SLOT) {
                fprintf(stderr, "cgrom_gen: %s は%d個まで指定できます\n", arg, MAX_FILES_PER_SLOT);
                *exit_code = 2;
                return 1;
            }
            opt->files[s][opt->count[s]++] = value;
        }
    }
    return 0;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "cgrom_gen: %s を作成できません\n", path);
        return 1;
    }
    size_t written = fwrite(data, 1, size, fp);
    int close_failed = fclose(fp) != 0;
    if (written != size || close_failed) {
        fprintf(stderr, "cgrom_gen: %s への書き込みに失敗しました\n", path);
        return 1;
    }
    return 0;
}

int cgrom_gen_main(int argc, char **argv) {
    options opt = {0};
    int exit_code;
    if (parse_args(argc, argv, &opt, &exit_code))
        return exit_code;
    if (opt.output == NULL) {
        fprintf(stderr, "cgrom_gen: --output を指定してください\n");
        print_usage(stderr);
        return 2;
    }
    int any = 0;
    for (int s = 0; s < SLOT_COUNT; s++)
        any |= opt.count[s] > 0;
    if (!any) {
        fprintf(stderr, "cgrom_gen: BDFを1つ以上指定してください\n");
        print_usage(stderr);
        return 2;
    }

    uint8_t *buf = calloc(CGROM_TOTAL_SIZE, 1);
    if (buf == NULL) {
        fprintf(stderr, "cgrom_gen: メモリを確保できません\n");
        return 1;
    }
    int status = 0;
    for (int s = 0; s < SLOT_COUNT && status == 0; s++) {
        if (opt.count[s] == 0) {
            fprintf(stderr, "cgrom_gen: %s（%s）は未指定のため空白にします\n", SLOTS[s].option, SLOTS[s].label);
            continue;
        }
        load_stats stats = {0};
        for (int f = 0; f < opt.count[s] && status == 0; f++)
            status = load_bdf(opt.files[s][f], (slot_id)s, buf, &stats);
        if (status == 0 && (opt.verbose || stats.placed == 0))
            fprintf(stderr, "%s: %u文字を取り込み（対象外 %u）\n", SLOTS[s].label, stats.placed, stats.skipped);
    }
    if (status == 0)
        status = write_file(opt.output, buf, CGROM_TOTAL_SIZE);
    if (status == 0)
        printf("wrote %s (%u bytes)\n", opt.output, CGROM_TOTAL_SIZE);
    free(buf);
    return status;
}

#ifndef CGROM_GEN_NO_MAIN
int main(int argc, char **argv) { return cgrom_gen_main(argc, argv); }
#endif
