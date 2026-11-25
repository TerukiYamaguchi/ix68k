/*
 * ANKフォント差し替え報告の単体テスト。
 *
 * x68k_bus_tvram_to_text() は本来CGROM ANKパターンをROM固定オフセットで
 * 参照するが、HIOCS PLUS等の常駐ドライバがIOCS $16(_FNTADR)を差し替えて
 * ANKフォントを実行時変更できることが判明した。ゲスト側の
 * 報告プログラム（tools-x68k/fntadr/fntadr.s）が実際に_FNTADRを呼び、
 * 結果を_B_PRINT経由で "FNTADR=XXXXXXXX;" として送ってくると、
 * x68k_bus_scan_fntadr_report() がそれを検出して x68k_bus_set_ank_font_override()
 * を呼び、以後 x68k_bus_tvram_to_text() はROM固定オフセットの代わりに
 * その実アドレス（base + ch*16）からパターンを読む。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEXT_COLS     128
#define TEXT_ROWS     64
#define TEXT_BUF_SIZE (TEXT_COLS * TEXT_ROWS + TEXT_ROWS + 1)

#define FIX_ROW 5
#define FIX_COL 10
/* テスト用のオーバーライド先頭アドレス（RAM上の適当な未使用領域）。 */
#define OVERRIDE_BASE 0x2000u

static const uint8_t PAT_ROM[16] = {0x18, 0x24, 0x24, 0x18, 0x24, 0x42, 0x42, 0x7E,
                                     0x42, 0x42, 0x24, 0x18, 0x00, 0x00, 0x00, 0x00};
static const uint8_t PAT_RAM[16] = {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81,
                                     0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF};

static void plant_rom_glyph(x68k_bus_t *b, uint8_t ch, const uint8_t pat[16]) {
    int idx = (ch + 0x80) & 0xFF; /* tvram_to_text の ch = (idx + 0x80) & 0xFF の逆 */
    memcpy(b->iocs_rom + 0x3A000 + idx * 16, pat, 16);
}

static void plant_ram_glyph(x68k_bus_t *b, uint32_t base, uint8_t ch, const uint8_t pat[16]) {
    memcpy(b->ram + base + (uint32_t)ch * 16, pat, 16);
}

static void write_cell(x68k_bus_t *b, int row, int col, const uint8_t pat[16]) {
    int scroll_y = x68k_bus_get_tvram_scroll_y(b);
    for (int y = 0; y < 16; y++) {
        int tvram_row = (row * 16 + y + scroll_y) & 1023;
        b->tvram[tvram_row * 128 + col] = pat[y]; /* plane0 */
    }
}

static char text_at(const x68k_bus_t *b, int row, int col) {
    static uint8_t out[TEXT_BUF_SIZE];
    int n = x68k_bus_tvram_to_text(b, TEXT_COLS, TEXT_ROWS, out, (int)sizeof(out));
    int i = 0;
    for (int line = 0; i < n && line < row; i++)
        if (out[i] == '\n')
            line++;
    for (int c = 0; i < n && out[i] != '\n'; i++, c++)
        if (c == col)
            return (char)out[i];
    return ' ';
}

static x68k_bus_t *make_bus(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    memset(b->iocs_rom, 0, IOCS_ROM_SIZE); /* CGROM照合の基準を確定させる */
    return b;
}

/* T1: オーバーライド未設定時はROM固定オフセットを使う（既存動作の回帰ガード）。 */
static void t1_no_override_uses_rom(void) {
    x68k_bus_t *b = make_bus();
    plant_rom_glyph(b, 'A', PAT_ROM);
    write_cell(b, FIX_ROW, FIX_COL, PAT_ROM);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'A');
    x68k_bus_destroy(b);
}

/* T2: オーバーライド設定後は、ROM側に別パターンが残っていても実アドレス
 * (base + ch*16) 側のパターンで照合する。 */
static void t2_override_uses_reported_address(void) {
    x68k_bus_t *b = make_bus();
    plant_rom_glyph(b, 'A', PAT_ROM); /* ROM側: 'A' はPAT_ROMのまま(古い内容) */
    plant_ram_glyph(b, OVERRIDE_BASE, 'A', PAT_RAM); /* 差し替え後: 'A' はPAT_RAM */
    write_cell(b, FIX_ROW, FIX_COL, PAT_RAM); /* 画面には新フォントで描画済み */
    x68k_bus_set_ank_font_override(b, OVERRIDE_BASE);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'A');
    x68k_bus_destroy(b);
}

/* T3: クリア後はROM固定オフセット参照に戻る（リセット時の挙動）。 */
static void t3_clear_reverts_to_rom(void) {
    x68k_bus_t *b = make_bus();
    plant_rom_glyph(b, 'A', PAT_ROM);
    plant_ram_glyph(b, OVERRIDE_BASE, 'A', PAT_RAM);
    write_cell(b, FIX_ROW, FIX_COL, PAT_ROM); /* 画面はROMパターンのまま */
    x68k_bus_set_ank_font_override(b, OVERRIDE_BASE);
    x68k_bus_clear_ank_font_override(b);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'A');
    x68k_bus_destroy(b);
}

/* T4: "FNTADR=XXXXXXXX;" マーカーを検出し、オーバーライドを適用する。 */
static void t4_scan_applies_marker(void) {
    x68k_bus_t *b = make_bus();
    plant_ram_glyph(b, OVERRIDE_BASE, 'A', PAT_RAM);
    write_cell(b, FIX_ROW, FIX_COL, PAT_RAM);
    const char *line = "FNTADR=00002000;\r\n";
    int applied = x68k_bus_scan_fntadr_report(b, (const uint8_t *)line, (int)strlen(line));
    assert(applied == 1);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'A');
    x68k_bus_destroy(b);
}

/* T5: マーカーの前後に無関係な文字列があっても検出できる。 */
static void t5_scan_finds_marker_with_noise(void) {
    x68k_bus_t *b = make_bus();
    plant_ram_glyph(b, OVERRIDE_BASE, 'A', PAT_RAM);
    write_cell(b, FIX_ROW, FIX_COL, PAT_RAM);
    const char *line = "hello FNTADR=00002000; world\r\n";
    int applied = x68k_bus_scan_fntadr_report(b, (const uint8_t *)line, (int)strlen(line));
    assert(applied == 1);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'A');
    x68k_bus_destroy(b);
}

/* T6: 不正な形式（16進以外の文字、終端の';'欠落）はオーバーライドを変更しない。 */
static void t6_scan_rejects_malformed_marker(void) {
    x68k_bus_t *b = make_bus();
    plant_rom_glyph(b, 'A', PAT_ROM);
    write_cell(b, FIX_ROW, FIX_COL, PAT_ROM);

    int applied1 = x68k_bus_scan_fntadr_report(b, (const uint8_t *)"FNTADR=0000200Z;",
                                                (int)strlen("FNTADR=0000200Z;"));
    assert(applied1 == 0);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'A'); /* オーバーライドは適用されない */

    int applied2 = x68k_bus_scan_fntadr_report(b, (const uint8_t *)"FNTADR=00002000",
                                                (int)strlen("FNTADR=00002000")); /* ';'なし */
    assert(applied2 == 0);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'A');
    x68k_bus_destroy(b);
}

int main(void) {
    t1_no_override_uses_rom();
    t2_override_uses_reported_address();
    t3_clear_reverts_to_rom();
    t4_scan_applies_marker();
    t5_scan_finds_marker_with_noise();
    t6_scan_rejects_malformed_marker();
    printf("ank_font_override_test: OK\n");
    return 0;
}
