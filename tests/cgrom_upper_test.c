/*
 * CGROM 上位256KB（$F80000-$FBFFFF）が IPLROM のミラーに隠れないことの
 * 単体テスト。24x24 漢字（第1水準末尾〜第2水準）は $F80000 以降にある。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CGROM_PATH      X68K_REPO_ROOT "/rom/CGROM_XEiJ.DAT"
#define CGROM_FILE_SIZE 0xC0000u /* 768KB */
#define ADDR_UPPER_HEAD 0xF81520u /* 腕($9872)の24x24字形 */
#define ADDR_UPPER_TAIL 0xFBFFFFu
#define ADDR_IPLROM     0xFF0000u
#define MARK_UPPER      0xA5u
#define MARK_TAIL       0x5Au
#define MARK_IPLROM     0x3Cu

static void check_all_paths(x68k_bus_t *b, uint32_t addr, uint8_t expect) {
    assert(x68k_bus_read8(b, addr) == expect);
    assert(x68k_bus_peek8(b, addr) == expect);
    assert((uint8_t)(x68k_bus_read16(b, addr & ~1u) >> ((addr & 1u) ? 0 : 8)) == expect);
}

/* T1: $F80000-$FBFFFF は iocs_rom のCGROM領域を直接返し、IPLROMと混ざらない。 */
static void t1_upper_cgrom_not_mirrored(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    memset(b->iocs_rom, 0, IOCS_ROM_SIZE);
    b->iocs_rom[ADDR_UPPER_HEAD - IOCS_ROM_BASE] = MARK_UPPER;
    b->iocs_rom[ADDR_UPPER_TAIL - IOCS_ROM_BASE] = MARK_TAIL;
    b->iocs_rom[IPLROM_BUF_OFFSET + (ADDR_IPLROM - 0xFE0000u)] = MARK_IPLROM;
    check_all_paths(b, ADDR_UPPER_HEAD, MARK_UPPER);
    check_all_paths(b, ADDR_UPPER_TAIL, MARK_TAIL);
    check_all_paths(b, ADDR_IPLROM, MARK_IPLROM);
    x68k_bus_destroy(b);
}

/* T2: 実CGROMを読み込み、$F00000-$FBFFFF の全バイトがファイルと一致する
 * （x68k_bus_load_cgrom の読込サイズと read8 のオフセット変換の両方を検証）。
 * CGROM.DAT は gitignore 対象の資産なので、無い環境ではスキップする。 */
static void t2_real_cgrom_matches_file(void) {
    FILE *f = fopen(CGROM_PATH, "rb");
    if (!f) {
        printf("cgrom_upper_test: T2 skipped (%s not found)\n", CGROM_PATH);
        return;
    }
    static uint8_t expect[CGROM_FILE_SIZE];
    size_t n = fread(expect, 1, sizeof(expect), f);
    fclose(f);
    assert(n == sizeof(expect));

    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    assert(x68k_bus_load_cgrom(b, CGROM_PATH) == 0);
    for (uint32_t off = 0; off < CGROM_FILE_SIZE; off++)
        assert(x68k_bus_peek8(b, IOCS_ROM_BASE + off) == expect[off]);
    x68k_bus_destroy(b);
}

int main(void) {
    t1_upper_cgrom_not_mirrored();
    t2_real_cgrom_matches_file();
    printf("cgrom_upper_test: OK\n");
    return 0;
}
