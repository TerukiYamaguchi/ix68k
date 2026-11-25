/*
 * _B_PUTMES キャプチャの賞味期限（描画後スナップショットとの一致判定）を
 * 検証する単体テスト。
 *
 * read_bitmap_text（x68k_bus_tvram_to_text）はキャプチャがあるセルでは
 * ピクセル解析より先にキャプチャ文字を採用するが、採用してよいのは
 * 「描画直後のセル内容（4プレーン×16スキャンライン）が現在のTVRAM内容と
 * 完全一致する＝まだ画面に出ている」間だけである。
 *
 * fixture設計上の注意: ピクセル解析のフォールバックはプレーン0だけを
 * CGROMと照合するため、キャプチャ文字とプレーン0のグリフを別の文字にして
 * おかないと「overlayが効いた」のか「フォールバックした」のか区別できない。
 * ここではキャプチャ文字を 'W''X''Y''Z'、プレーン0に植えるグリフを 'B' と
 * している（CGROM未登録パターンを書いた場合は '?' が返る）。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* CGROMに 'B' として植えるグリフ。 */
static const uint8_t PAT_B[16] = {0x18, 0x24, 0x24, 0x18, 0x24, 0x42, 0x42, 0x7E,
                                  0x42, 0x42, 0x24, 0x18, 0x00, 0x00, 0x00, 0x00};
/* CGROMに存在しないパターン（照合失敗 → '?'）。 */
static const uint8_t PAT_OTHER[16] = {0xFF, 0x81, 0xFF, 0x81, 0xFF, 0x81, 0xFF, 0x81,
                                      0xFF, 0x81, 0xFF, 0x81, 0xFF, 0x81, 0xFF, 0x81};
/* プレーン1-3に置く任意の非ゼロパターン（ピクセル解析には使われない）。 */
static const uint8_t PAT_P1[16] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
                                   0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};
static const uint8_t PAT_P2[16] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01,
                                   0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
static const uint8_t PAT_P3[16] = {0x0F, 0x0F, 0xF0, 0xF0, 0x0F, 0x0F, 0xF0, 0xF0,
                                   0x0F, 0x0F, 0xF0, 0xF0, 0x0F, 0x0F, 0xF0, 0xF0};

#define TEXT_COLS     128
#define TEXT_ROWS     64
#define TEXT_BUF_SIZE (TEXT_COLS * TEXT_ROWS + TEXT_ROWS + 1)

#define FIX_ROW 5
#define FIX_COL 10
#define FIX_LEN 4
static const char FIX_TEXT[FIX_LEN] = {'W', 'X', 'Y', 'Z'};

static void plant_glyph(x68k_bus_t *b, uint8_t ch, const uint8_t pat[16]) {
    int idx = (ch + 0x80) & 0xFF; /* tvram_to_text の ch = (idx + 0x80) & 0xFF の逆 */
    memcpy(b->iocs_rom + 0x3A000 + idx * 16, pat, 16);
}

static void write_cell(x68k_bus_t *b, int plane, int row, int col, const uint8_t pat[16]) {
    int scroll_y = x68k_bus_get_tvram_scroll_y(b);
    for (int y = 0; y < 16; y++) {
        int tvram_row = (row * 16 + y + scroll_y) & 1023;
        b->tvram[(uint32_t)plane * TVRAM_PLANE_SIZE + tvram_row * 128 + col] = pat[y];
    }
}

/* (row,col) セルが read_bitmap_text で何の文字として読めるかを返す
 * （行末の空白は削られるため、行が短ければ空白扱い）。 */
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
    /* iocs_rom は malloc されただけで未初期化。CGROM照合の基準を確定させる。 */
    memset(b->iocs_rom, 0, IOCS_ROM_SIZE);
    plant_glyph(b, 'B', PAT_B);
    return b;
}

/* FIX範囲の4セルに「プレーン0='B'のグリフ、プレーン1-3は非ゼロパターン」を書き、
 * キャプチャ "WXYZ" を記録する。overlay有効なら "WXYZ"、失効すれば 'B' が返る。 */
static void setup_fixture(x68k_bus_t *b) {
    for (int k = 0; k < FIX_LEN; k++) {
        write_cell(b, 0, FIX_ROW, FIX_COL + k, PAT_B);
        write_cell(b, 1, FIX_ROW, FIX_COL + k, PAT_P1);
        write_cell(b, 2, FIX_ROW, FIX_COL + k, PAT_P2);
        write_cell(b, 3, FIX_ROW, FIX_COL + k, PAT_P3);
    }
    x68k_bus_record_putmes(b, FIX_ROW, FIX_COL, FIX_LEN, FIX_TEXT);
}

static void tick(x68k_bus_t *b, int n) {
    for (int i = 0; i < n; i++)
        x68k_bus_putmes_snapshot_tick(b);
}

static void assert_fixture_captured(const x68k_bus_t *b) {
    for (int k = 0; k < FIX_LEN; k++)
        assert(text_at(b, FIX_ROW, FIX_COL + k) == FIX_TEXT[k]);
}

/* T1: 描画内容が変わっていなければキャプチャが優先される（回帰ガード）。 */
static void t1_capture_wins(void) {
    x68k_bus_t *b = make_bus();
    setup_fixture(b);
    tick(b, 2);
    assert_fixture_captured(b);
    x68k_bus_destroy(b);
}

/* T2: 4プレーンのどれか1つでも書き換わればキャプチャは失効する。 */
static void t2_any_plane_change_invalidates(void) {
    for (int plane = 0; plane < 4; plane++) {
        x68k_bus_t *b = make_bus();
        setup_fixture(b);
        tick(b, 2);
        write_cell(b, plane, FIX_ROW, FIX_COL, PAT_OTHER);
        /* プレーン0を壊した場合はCGROM未登録の '?'、それ以外のプレーンなら
         * プレーン0のグリフがそのまま読めて 'B' になる。いずれもキャプチャ
         * 文字 'W' ではない＝失効している。 */
        assert(text_at(b, FIX_ROW, FIX_COL) == (plane == 0 ? '?' : 'B'));
        x68k_bus_destroy(b);
    }
}

/* T3: 同一行を桁違いの複数PUTMESで分割構成するケースを壊さない
 * （「無効なメディア」ダイアログの中止/再実行/無視行が読めなくなる回帰の防止）。 */
static void t3_multiple_segments_same_row(void) {
    x68k_bus_t *b = make_bus();
    const int cols[3] = {0, 20, 40};
    const char *seg[3] = {"abcd", "efgh", "ijkl"};
    for (int s = 0; s < 3; s++) {
        for (int k = 0; k < 4; k++)
            write_cell(b, 0, FIX_ROW, cols[s] + k, PAT_B);
        x68k_bus_record_putmes(b, FIX_ROW, cols[s], 4, seg[s]);
    }
    tick(b, 2);
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < 4; k++)
            assert(text_at(b, FIX_ROW, cols[s] + k) == seg[s][k]);
    x68k_bus_destroy(b);
}

/* T4: 同一(行,開始桁)へ幅を変えて再記録したとき、旧エントリの末尾桁が残らない。 */
static void t4_same_origin_shorter_rewrite(void) {
    x68k_bus_t *b = make_bus();
    for (int k = 0; k < 8; k++)
        write_cell(b, 0, FIX_ROW, FIX_COL + k, PAT_B);
    x68k_bus_record_putmes(b, FIX_ROW, FIX_COL, 8, "OLDOLDOL");
    x68k_bus_record_putmes(b, FIX_ROW, FIX_COL, 4, "NEW!");
    tick(b, 2);
    const char *expect = "NEW!";
    for (int k = 0; k < 4; k++)
        assert(text_at(b, FIX_ROW, FIX_COL + k) == expect[k]);
    for (int k = 4; k < 8; k++)
        assert(text_at(b, FIX_ROW, FIX_COL + k) == 'B'); /* 旧エントリは残らない */
    x68k_bus_destroy(b);
}

/* T5: 未スナップショット（記録直後）のエントリは検証せず採用する。 */
static void t5_unsnapped_is_trusted(void) {
    x68k_bus_t *b = make_bus();
    setup_fixture(b);
    assert_fixture_captured(b); /* tick前 */
    x68k_bus_destroy(b);
}

/* T6: 失効はセル単位。壊れたセルだけがフォールバックし、他は残る。 */
static void t6_per_cell_invalidation(void) {
    x68k_bus_t *b = make_bus();
    setup_fixture(b);
    tick(b, 2);
    write_cell(b, 2, FIX_ROW, FIX_COL + 1, PAT_OTHER); /* 1セルのプレーン2だけ壊す */
    assert(text_at(b, FIX_ROW, FIX_COL + 0) == 'W');
    assert(text_at(b, FIX_ROW, FIX_COL + 1) == 'B'); /* このセルだけフォールバック */
    assert(text_at(b, FIX_ROW, FIX_COL + 2) == 'Y');
    assert(text_at(b, FIX_ROW, FIX_COL + 3) == 'Z');
    x68k_bus_destroy(b);
}

/* T7: 非空白を記録したのに覆うセルが4プレーンとも全ゼロなら、
 * 猶予期間内に消えたとみなして即座に無効化する。 */
static void t7_all_zero_cells_invalidate(void) {
    x68k_bus_t *b = make_bus();
    x68k_bus_record_putmes(b, FIX_ROW, FIX_COL, FIX_LEN, FIX_TEXT);
    tick(b, 2);
    assert(b->putmes_ring[0].valid == 0);
    assert(text_at(b, FIX_ROW, FIX_COL) == ' '); /* 全ゼロセル＝空白 */
    x68k_bus_destroy(b);
}

/* T8: リセット時のクリアでキャプチャが残らない。 */
static void t8_clear_drops_captures(void) {
    x68k_bus_t *b = make_bus();
    setup_fixture(b);
    tick(b, 2);
    x68k_bus_putmes_clear(b);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'B');
    x68k_bus_destroy(b);
}

/* T9: 記録フレーム自身の境界（tick 1回目）ではまだスナップショットしない。 */
static void t9_first_tick_does_not_snapshot(void) {
    x68k_bus_t *b = make_bus();
    setup_fixture(b);
    tick(b, 1);
    write_cell(b, 1, FIX_ROW, FIX_COL, PAT_OTHER); /* 壊しても検証基準がまだ無い */
    assert(b->putmes_ring[0].snapped == 0);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'W');
    x68k_bus_destroy(b);
}

/* T10: 2回目のtickで初めてスナップショットが成立し、以後は検証される。 */
static void t10_second_tick_snapshots(void) {
    x68k_bus_t *b = make_bus();
    setup_fixture(b);
    tick(b, 2);
    assert(b->putmes_ring[0].snapped == 1);
    write_cell(b, 1, FIX_ROW, FIX_COL, PAT_OTHER);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'B');
    x68k_bus_destroy(b);
}

/* T11: フレームカウンタの32bitラップアラウンドを跨いでも判定が成立する
 * （符号無し差分でなく `>` 比較で実装するとここで失敗する）。 */
static void t11_frame_counter_wraparound(void) {
    x68k_bus_t *b = make_bus();
    b->putmes_frame_counter = 0xFFFFFFFFu - 1u;
    setup_fixture(b);
    tick(b, 2); /* カウンタが 0xFFFFFFFF → 0 へラップする */
    assert(b->putmes_ring[0].snapped == 1);
    assert_fixture_captured(b); /* 内容が変わっていないので有効なまま */
    write_cell(b, 3, FIX_ROW, FIX_COL, PAT_OTHER);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'B'); /* 変わったら失効する */
    x68k_bus_destroy(b);
}

/* T12: col+len が桁数上限を超える記録は len がクランプされ、
 * 行/桁の端（スクロールのラップを跨ぐ位置）でも範囲外を読まない。 */
static void t12_col_len_clamp(void) {
    x68k_bus_t *b = make_bus();
    b->crtc_regs[0x16] = 0x00; /* R11 = 1: (row*16+y+scroll_y) が 1023→0 を跨ぐ */
    b->crtc_regs[0x17] = 0x01;
    char text[PUTMES_TEXT_MAX];
    memset(text, 'Q', sizeof(text));
    write_cell(b, 0, 63, 127, PAT_B);
    x68k_bus_record_putmes(b, 63, 127, PUTMES_TEXT_MAX, text);
    assert(b->putmes_ring[0].len == 1); /* 128 - 127 */
    tick(b, 2);
    assert(text_at(b, 63, 127) == 'Q');
    x68k_bus_destroy(b);
}

/* T13: クリア後に記録しても（カウンタをリセットしない設計のまま）
 * 2フレーム後に検証が有効化される。 */
static void t13_record_after_clear(void) {
    x68k_bus_t *b = make_bus();
    setup_fixture(b);
    tick(b, 2);
    x68k_bus_putmes_clear(b);
    setup_fixture(b);
    tick(b, 2);
    assert(b->putmes_ring[0].snapped == 1);
    assert_fixture_captured(b);
    write_cell(b, 1, FIX_ROW, FIX_COL, PAT_OTHER);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'B');
    x68k_bus_destroy(b);
}

/* T14: 判定はtickの呼ばれた回数だけで進む（tick間の実時間には依存しない）。
 * デバッガ停止でtickが呼ばれない期間があっても、再開後の2回目のtickで
 * スナップショットが成立する。 */
static void t14_tick_count_only(void) {
    x68k_bus_t *b = make_bus();
    setup_fixture(b);
    tick(b, 1);
    assert(b->putmes_ring[0].snapped == 0); /* ここで長時間停止していても同じ */
    tick(b, 1);
    assert(b->putmes_ring[0].snapped == 1);
    write_cell(b, 1, FIX_ROW, FIX_COL, PAT_OTHER);
    assert(text_at(b, FIX_ROW, FIX_COL) == 'B');
    x68k_bus_destroy(b);
}

/* T15: 隣接セルが別々のエントリ・別々のパターンでも取り違えない。 */
static void t15_adjacent_cells_independent(void) {
    x68k_bus_t *b = make_bus();
    write_cell(b, 0, FIX_ROW, 10, PAT_B);
    write_cell(b, 1, FIX_ROW, 10, PAT_P1);
    write_cell(b, 0, FIX_ROW, 11, PAT_B);
    write_cell(b, 1, FIX_ROW, 11, PAT_P2);
    x68k_bus_record_putmes(b, FIX_ROW, 10, 1, "1");
    x68k_bus_record_putmes(b, FIX_ROW, 11, 1, "2");
    tick(b, 2);
    assert(text_at(b, FIX_ROW, 10) == '1');
    assert(text_at(b, FIX_ROW, 11) == '2');
    /* 片方だけ壊しても、もう片方は生きている。 */
    write_cell(b, 1, FIX_ROW, 10, PAT_OTHER);
    assert(text_at(b, FIX_ROW, 10) == 'B');
    assert(text_at(b, FIX_ROW, 11) == '2');
    x68k_bus_destroy(b);
}

int main(void) {
    t1_capture_wins();
    t2_any_plane_change_invalidates();
    t3_multiple_segments_same_row();
    t4_same_origin_shorter_rewrite();
    t5_unsnapped_is_trusted();
    t6_per_cell_invalidation();
    t7_all_zero_cells_invalidate();
    t8_clear_drops_captures();
    t9_first_tick_does_not_snapshot();
    t10_second_tick_snapshots();
    t11_frame_counter_wraparound();
    t12_col_len_clamp();
    t13_record_after_clear();
    t14_tick_count_only();
    t15_adjacent_cells_independent();
    printf("putmes_overlay_test: OK\n");
    return 0;
}
