#include "app/rom_check.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_SIZE_IPLROM 0x20000u
#define ROM_SIZE_FULL 0x100000u
#define VECTOR_OFFSET_IPLROM 0x10000u
#define PC_68000_CLASS 0xFF0010u
#define PC_030_CLASS 0xFF0038u

/* ベクタ付き ROM を作る。vector_offset の SSP=$2000, PC=pc。 */
static void write_rom(const char *path, size_t size, size_t vector_offset, uint32_t pc) {
    uint8_t *buf = calloc(size, 1);
    assert(buf);
    if (size >= vector_offset + 8) {
        const uint32_t ssp = 0x2000;
        for (int i = 0; i < 4; i++) {
            buf[vector_offset + i] = (uint8_t)(ssp >> (24 - 8 * i));
            buf[vector_offset + 4 + i] = (uint8_t)(pc >> (24 - 8 * i));
        }
    }
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(buf, 1, size, f) == size);
    fclose(f);
    free(buf);
}

static x68k_rom_check_result_t check(const char *path, x68k_timing_profile_t profile,
                                     mc68k_model_t cpu) {
    long size = 0;
    return x68k_rom_check(path, profile, cpu, &size);
}

int main(void) {
    const char *rom68k = "IPLROM.DAT";
    const char *romxv = "iplromxv.dat";
    const char *rom030 = "IPLROM30.DAT";
    const char *romnew = "IPLROM1.7"; /* 未知の名前は系統を問わず受け入れる */
    const char *romfull = "rom_check_full.bin";
    const char *rom256k = "rom_check_256k.dat";
    const char *romscsi = "rom_check_scsiinrom.dat";
    const char *romempty = "rom_check_empty.dat";
    const char *romcg = "rom_check_cgrom.dat";
    const char *missing = "rom_check_missing.dat";

    write_rom(rom68k, ROM_SIZE_IPLROM, VECTOR_OFFSET_IPLROM, PC_68000_CLASS);
    write_rom(romxv, ROM_SIZE_IPLROM, VECTOR_OFFSET_IPLROM, PC_68000_CLASS);
    write_rom(romnew, ROM_SIZE_IPLROM, VECTOR_OFFSET_IPLROM, PC_68000_CLASS);
    write_rom(rom030, ROM_SIZE_IPLROM, VECTOR_OFFSET_IPLROM, PC_030_CLASS);
    write_rom(romfull, ROM_SIZE_FULL, 0, 0xFF0010u);
    write_rom(rom256k, 0x40000, VECTOR_OFFSET_IPLROM, PC_68000_CLASS);
    write_rom(romscsi, 0x2000, 0, PC_68000_CLASS);
    write_rom(romempty, 0, 0, 0);
    write_rom(romcg, ROM_SIZE_IPLROM, VECTOR_OFFSET_IPLROM, 0); /* PC=0: ROM 領域外 */
    (void)romcg;

    /* 正常系 */
    assert(check(rom68k, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) == X68K_ROM_CHECK_OK);
    assert(check(romxv, X68K_TIMING_PROFILE_XVI, MC68K_MODEL_68000) == X68K_ROM_CHECK_OK);
    assert(check(rom030, X68K_TIMING_PROFILE_X68030, MC68K_MODEL_68EC030) == X68K_ROM_CHECK_OK);
    assert(check(rom030, X68K_TIMING_PROFILE_060TURBO, MC68K_MODEL_68060) == X68K_ROM_CHECK_OK);
    assert(check(rom030, X68K_TIMING_PROFILE_040TURBO, MC68K_MODEL_68040) == X68K_ROM_CHECK_OK);
    assert(check(romfull, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) == X68K_ROM_CHECK_OK);
    /* X68000 と XVI は同じ系統として扱う */
    assert(check(rom68k, X68K_TIMING_PROFILE_XVI, MC68K_MODEL_68000) == X68K_ROM_CHECK_OK);

    /* 読めない・サイズ・ベクタ */
    assert(check(missing, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) ==
           X68K_ROM_CHECK_UNREADABLE);
    assert(check(romempty, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) ==
           X68K_ROM_CHECK_UNREADABLE);
    assert(check(".", X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) ==
           X68K_ROM_CHECK_UNREADABLE);
    assert(check(rom256k, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) ==
           X68K_ROM_CHECK_BAD_SIZE);
    assert(check(romscsi, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) ==
           X68K_ROM_CHECK_BAD_SIZE);
    assert(check(romcg, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) ==
           X68K_ROM_CHECK_BAD_VECTOR);

    /* CPU 不一致（bus_timing = off でも検出） */
    assert(check(rom030, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) ==
           X68K_ROM_CHECK_CPU_MISMATCH);
    assert(check(rom030, X68K_TIMING_PROFILE_NONE, MC68K_MODEL_68000) ==
           X68K_ROM_CHECK_CPU_MISMATCH);

    /* ファイル名の機種系統と series の不一致 */
    assert(check(rom68k, X68K_TIMING_PROFILE_X68030, MC68K_MODEL_68030) ==
           X68K_ROM_CHECK_SERIES_MISMATCH);
    assert(check(rom68k, X68K_TIMING_PROFILE_060TURBO, MC68K_MODEL_68060) ==
           X68K_ROM_CHECK_SERIES_MISMATCH);
    assert(check(rom030, X68K_TIMING_PROFILE_XVI, MC68K_MODEL_68030) ==
           X68K_ROM_CHECK_SERIES_MISMATCH);
    /* bus_timing = off は series と照合しない。未知のファイル名も照合しない */
    assert(check(rom68k, X68K_TIMING_PROFILE_NONE, MC68K_MODEL_68030) == X68K_ROM_CHECK_OK);
    assert(check(romfull, X68K_TIMING_PROFILE_X68030, MC68K_MODEL_68030) == X68K_ROM_CHECK_OK);

    /* 未知のファイル名（新版 ROM など）はどの series でも受け入れる */
    assert(check(romnew, X68K_TIMING_PROFILE_X68000, MC68K_MODEL_68000) == X68K_ROM_CHECK_OK);
    assert(check(romnew, X68K_TIMING_PROFILE_X68030, MC68K_MODEL_68030) == X68K_ROM_CHECK_OK);

    /* CGROM: 128KB の IPLROM だけが必要とする */
    assert(x68k_cgrom_required(ROM_SIZE_IPLROM));
    assert(!x68k_cgrom_required(ROM_SIZE_FULL));

    /* メッセージ */
    char buf[512];
    x68k_rom_check_format(X68K_ROM_CHECK_BAD_SIZE, "x.dat", 262144, buf, sizeof(buf));
    assert(strstr(buf, "262144"));
    x68k_cgrom_error_format(NULL, buf, sizeof(buf));
    assert(strstr(buf, "指定されていません"));
    x68k_cgrom_error_format("", buf, sizeof(buf));
    assert(strstr(buf, "指定されていません"));
    x68k_cgrom_error_format("c.dat", buf, sizeof(buf));
    assert(strstr(buf, "c.dat") && strstr(buf, "読み込めません"));
    x68k_rom_check_format(X68K_ROM_CHECK_OK, "x.dat", 0, buf, sizeof(buf));
    assert(buf[0] == '\0');

    const char *files[] = {rom68k, romxv, rom030, romfull, rom256k, romnew, romscsi, romempty, romcg};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        remove(files[i]);
    puts("rom_check_test: OK");
    return 0;
}
