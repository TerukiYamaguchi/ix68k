#include "app/rom_check.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ROM_SIZE_IPLROM          0x20000L  /* 128KB: 0xFC0000-0xFDFFFF 相当 */
#define ROM_SIZE_FULL            0x100000L /* 1MB: 0xF00000 からのフルイメージ */
#define ROM_VECTOR_OFFSET_IPLROM 0x10000L  /* IPLROM は後半 64KB の先頭がリセットベクタ */
#define ROM_VECTOR_SIZE          8
#define ROM_AREA_FULL_BASE       0xF00000u
#define ROM_AREA_IPLROM_BASE     0xFF0000u
#define ROM_AREA_END             0xFFFFFFu
#define ROM_PC_030_CLASS         0xFF0038u /* IPLROM30 / IPLROM16 のリセット PC */

typedef enum {
    ROM_NAME_UNKNOWN,
    ROM_NAME_68000_CLASS, /* IPLROM / IPLROMCO / IPLROMXV */
    ROM_NAME_030_CLASS,   /* IPLROM30 / IPLROM16 */
} rom_name_class_t;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

static int has_prefix_nocase(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != *prefix)
            return 0;
    return 1;
}

static rom_name_class_t classify_name(const char *path) {
    const char *name = base_name(path);
    if (has_prefix_nocase(name, "iplrom30") || has_prefix_nocase(name, "iplrom16"))
        return ROM_NAME_030_CLASS;
    if (has_prefix_nocase(name, "iplromxv") || has_prefix_nocase(name, "iplromco") ||
        has_prefix_nocase(name, "iplrom.dat"))
        return ROM_NAME_68000_CLASS;
    return ROM_NAME_UNKNOWN;
}

static int profile_is_030_class(x68k_timing_profile_t profile) {
    return profile == X68K_TIMING_PROFILE_X68030 || profile == X68K_TIMING_PROFILE_060TURBO ||
           profile == X68K_TIMING_PROFILE_040TURBO;
}

x68k_rom_check_result_t x68k_rom_check(const char *path, x68k_timing_profile_t profile,
                                       mc68k_model_t cpu_model, long *size_out) {
    if (size_out)
        *size_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return X68K_ROM_CHECK_UNREADABLE;
    /* ディレクトリは fopen が成功する環境があるため、1 バイト読んで確かめる。 */
    if (fgetc(f) == EOF && ferror(f)) {
        fclose(f);
        return X68K_ROM_CHECK_UNREADABLE;
    }
    rewind(f);
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        size = ftell(f);
    if (size < 0) {
        fclose(f);
        return X68K_ROM_CHECK_UNREADABLE;
    }
    if (size_out)
        *size_out = size;
    if (size != ROM_SIZE_IPLROM && size != ROM_SIZE_FULL) {
        fclose(f);
        return size == 0 ? X68K_ROM_CHECK_UNREADABLE : X68K_ROM_CHECK_BAD_SIZE;
    }

    const long vector_offset = size == ROM_SIZE_IPLROM ? ROM_VECTOR_OFFSET_IPLROM : 0;
    uint8_t vec[ROM_VECTOR_SIZE];
    const int read_ok =
        fseek(f, vector_offset, SEEK_SET) == 0 && fread(vec, 1, sizeof(vec), f) == sizeof(vec);
    fclose(f);
    if (!read_ok)
        return X68K_ROM_CHECK_UNREADABLE;

    const uint32_t pc = be32(vec + 4);
    const uint32_t area_base =
        size == ROM_SIZE_IPLROM ? ROM_AREA_IPLROM_BASE : ROM_AREA_FULL_BASE;
    if (pc < area_base || pc > ROM_AREA_END || (pc & 1u))
        return X68K_ROM_CHECK_BAD_VECTOR;

    if (pc == ROM_PC_030_CLASS &&
        (cpu_model == MC68K_MODEL_68000 || cpu_model == MC68K_MODEL_68010))
        return X68K_ROM_CHECK_CPU_MISMATCH;

    if (profile != X68K_TIMING_PROFILE_NONE) {
        const rom_name_class_t name_class = classify_name(path);
        const rom_name_class_t series_class =
            profile_is_030_class(profile) ? ROM_NAME_030_CLASS : ROM_NAME_68000_CLASS;
        if (name_class != ROM_NAME_UNKNOWN && name_class != series_class)
            return X68K_ROM_CHECK_SERIES_MISMATCH;
    }
    return X68K_ROM_CHECK_OK;
}

void x68k_rom_check_format(x68k_rom_check_result_t result, const char *path, long size,
                           char *buf, size_t buf_size) {
    /* 形式: 問題 → 空行 → ファイル → 空行 → 対処（箇条書き）。1 行は全角 30 字程度まで。 */
    switch (result) {
    case X68K_ROM_CHECK_OK:
        snprintf(buf, buf_size, "%s", "");
        break;
    case X68K_ROM_CHECK_UNREADABLE:
        snprintf(buf, buf_size,
                 "IPLROM を読み込めません。\n\n"
                 "  ファイル: %s\n\n"
                 "次を確認してください。\n"
                 "  - ファイルが存在する\n"
                 "  - ファイルが空ではない\n"
                 "  - ディレクトリではなくファイルを指定している",
                 path);
        break;
    case X68K_ROM_CHECK_BAD_SIZE:
        snprintf(buf, buf_size,
                 "IPLROM のサイズが不正です。\n\n"
                 "  ファイル: %s\n"
                 "  サイズ: %ld バイト\n\n"
                 "128KB（131072 バイト）または 1MB の\n"
                 "IPLROM を指定してください。\n"
                 "256KB 版や SCSIINROM 単体は使えません。",
                 path, size);
        break;
    case X68K_ROM_CHECK_BAD_VECTOR:
        snprintf(buf, buf_size,
                 "IPLROM ではないファイルが指定されています。\n\n"
                 "  ファイル: %s\n\n"
                 "リセットベクタが ROM 領域を指していません。\n"
                 "CGROM などを rom に指定していないか\n"
                 "確認してください。",
                 path);
        break;
    case X68K_ROM_CHECK_CPU_MISMATCH:
        snprintf(buf, buf_size,
                 "この IPLROM は X68030 用で、\n"
                 "68000 / 68010 では動きません。\n\n"
                 "  ファイル: %s\n\n"
                 "次のどちらかにしてください。\n"
                 "  - series = x68030 を指定する\n"
                 "  - X68000 用の IPLROM に変更する",
                 path);
        break;
    case X68K_ROM_CHECK_SERIES_MISMATCH:
        snprintf(buf, buf_size,
                 "IPLROM の機種と series が一致しません。\n\n"
                 "  ファイル: %s\n\n"
                 "対応は次のとおりです。\n"
                 "  - IPLROM / IPLROMXV / IPLROMCO\n"
                 "    → series = x68000 または xvi\n"
                 "  - IPLROM30 / IPLROM16\n"
                 "    → series = x68030",
                 path);
        break;
    }
}

int x68k_cgrom_required(long rom_size) {
    return rom_size == ROM_SIZE_IPLROM;
}

void x68k_cgrom_error_format(const char *path, char *buf, size_t buf_size) {
    if (!path || !path[0]) {
        snprintf(buf, buf_size,
                 "CGROM が指定されていません。\n\n"
                 "文字を表示するには CGROM が必要です。\n\n"
                 "次のどちらかで指定してください。\n"
                 "  - -cgrom オプション\n"
                 "  - 設定ファイルの cgrom =");
        return;
    }
    snprintf(buf, buf_size,
             "CGROM を読み込めません。\n\n"
             "  ファイル: %s\n\n"
             "次を確認してください。\n"
             "  - ファイルが存在する\n"
             "  - ファイルが空ではない\n"
             "  - ディレクトリではなくファイルを指定している",
             path);
}
