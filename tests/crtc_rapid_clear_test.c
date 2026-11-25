/*
 * CRTC動作ポート ($E80481) bit1 のグラフィック画面高速クリアの単体テスト。
 *x68k_bus_write16()/read16() 経由でポートを叩き、
 * x68k_bus_crtc_rapid_clear() が選択プレーン・表示高さ・ページ別Yスクロールを
 * 正しく反映してGVRAMをクリアすること、完了印としてbit1が自動的に0へ戻ることを
 * 検証する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_reg(x68k_bus_t *b, int reg_num, uint16_t val) {
    b->crtc_regs[reg_num * 2] = (uint8_t)(val >> 8);
    b->crtc_regs[reg_num * 2 + 1] = (uint8_t)(val & 0xFFu);
}

/* R20: crtc_regs[0x28]は色モード判定に使われないため固定の非ゼロ値、
 * crtc_regs[0x29]はVDビット(bit3-2)で表示高さ(0=256/1=512/2=1024)を決める。
 * 全ゼロだと512x512フォールバックが働いてしまうため、hi常に非ゼロにしておく。 */
static void set_display_height(x68k_bus_t *b, int vd) {
    b->crtc_regs[0x28] = 0x01u;
    b->crtc_regs[0x29] = (uint8_t)(vd << 2);
}

/* ビデオコントローラ R0($E82400) の GrMM(bit2-0)。色数・実画面サイズの一次情報。
 * 0=512ドット16色 / 1=512ドット256色 / 3=512ドット65536色 / 4=1024ドット16色 */
static void set_memory_mode(x68k_bus_t *b, uint8_t grmm) {
    b->crtc_regs[0x2401u] = (uint8_t)(grmm & 0x07u);
}

static void set_cc(x68k_bus_t *b, uint8_t mask) {
    b->crtc_regs[0x2Bu] = (uint8_t)(mask & 0x0Fu);
}

static void fill_gvram(x68k_bus_t *b, uint8_t val) {
    memset(b->gvram, val, GVRAM_SIZE);
}

static int gvram_row_is(const x68k_bus_t *b, uint32_t base, int row, uint32_t row_bytes,
                         uint8_t val) {
    const uint8_t *p = b->gvram + base + (uint32_t)row * row_bytes;
    for (uint32_t i = 0; i < row_bytes; i++)
        if (p[i] != val)
            return 0;
    return 1;
}

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* T1: word write経由で発火する。512dot/16色4ページ(GrMM=0), h=512, CP=$F, スクロール0 */
    set_memory_mode(b, 0);
    set_display_height(b, 1); /* VD=01 → h=512 */
    set_cc(b, 0x0F);
    for (int r = 0; r < 4; r++)
        set_reg(b, 13 + 2 * r, 0);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    for (uint32_t off = 0; off < GVRAM_SIZE; off++)
        assert(b->gvram[off] == 0x00);

    /* T2: 非選択プレーン不変。CP=$5(plane0,2のみ) */
    set_cc(b, 0x05);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    for (uint32_t off = 0x00000; off < 0x80000; off++)
        assert(b->gvram[off] == 0x00); /* plane0 */
    for (uint32_t off = 0x80000; off < 0x100000; off++)
        assert(b->gvram[off] == 0xAA); /* plane1: 不変 */
    for (uint32_t off = 0x100000; off < 0x180000; off++)
        assert(b->gvram[off] == 0x00); /* plane2 */
    for (uint32_t off = 0x180000; off < 0x200000; off++)
        assert(b->gvram[off] == 0xAA); /* plane3: 不変 */

    /* T3: 表示高さが小さいモードで表示外ラスタが保持される。
     * 512dot/16色4ページ(GrMM=0), h=256(VD=00), CP=$1(plane0のみ) */
    set_memory_mode(b, 0);
    set_display_height(b, 0); /* VD=00 → h=256 */
    set_cc(b, 0x01);
    for (int r = 0; r < 4; r++)
        set_reg(b, 13 + 2 * r, 0);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    for (int row = 0; row < 256; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0x00));
    for (int row = 256; row < 512; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0xAA));

    /* T4: Yスクロール加味（非ラップ）。R13=100 */
    set_reg(b, 13, 100);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    for (int row = 0; row < 100; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0xAA));
    for (int row = 100; row < 356; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0x00));
    for (int row = 356; row < 512; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0xAA));

    /* T5: Yスクロール加味（球面ラップ）。R13=400 → (y+400)&511 */
    set_reg(b, 13, 400);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    for (int row = 0; row < 144; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0x00));
    for (int row = 144; row < 400; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0xAA));
    for (int row = 400; row < 512; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0x00));

    /* T6: プレーン別スクロールの添字対応。R13=0/R15=8/R17=16/R19=24、CP=$F
     * → プレーンpの消去開始行が8*p */
    set_reg(b, 13, 0);
    set_reg(b, 15, 8);
    set_reg(b, 17, 16);
    set_reg(b, 19, 24);
    set_cc(b, 0x0F);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    static const uint32_t plane_base[4] = {0x000000u, 0x080000u, 0x100000u, 0x180000u};
    static const int plane_start[4] = {0, 8, 16, 24};
    for (int p = 0; p < 4; p++) {
        int start = plane_start[p];
        assert(gvram_row_is(b, plane_base[p], start - 1 < 0 ? 511 : start - 1, 1024, 0xAA));
        assert(gvram_row_is(b, plane_base[p], start, 1024, 0x00));
        assert(gvram_row_is(b, plane_base[p], start + 255, 1024, 0x00));
        assert(gvram_row_is(b, plane_base[p], start + 256, 1024, 0xAA));
    }

    /* T7: 1024ドットモードはCPを無視する。1024dot/16色(GrMM=4), h=1024(VD=10),
     * CP=$0（本来なら何も選択していないはずだが、1024ドットモードでは無視されて消える） */
    set_memory_mode(b, 4);
    set_display_height(b, 2); /* VD=10 → h=1024 */
    set_cc(b, 0x00);
    set_reg(b, 13, 0);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    for (uint32_t off = 0; off < GVRAM_SIZE; off++)
        assert(b->gvram[off] == 0x00);

    /* T8: 1024ドットモードの行ストライドとマスク。h=256(VD=00), R13=1000
     * → (y+1000)&1023、1行=2048バイト */
    set_display_height(b, 0); /* VD=00 → h=256 */
    set_reg(b, 13, 1000);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    for (int row = 0; row < 232; row++)
        assert(gvram_row_is(b, 0x000000, row, 2048, 0x00));
    for (int row = 232; row < 1000; row++)
        assert(gvram_row_is(b, 0x000000, row, 2048, 0xAA));
    for (int row = 1000; row < 1024; row++)
        assert(gvram_row_is(b, 0x000000, row, 2048, 0x00));

    /* T9: CP=$0でも「1回目の読み出しは1、2回目は0」の保留状態になる
     * （実機/XEiJはCPに関わらずトリガ後は必ず1フレーム busy になるため。
     * 無限ループ回避の本質は「1が一度も観測できないと2段階ポーリングが詰まる」こと）。
     * 512ドットモードに戻す */
    set_memory_mode(b, 0);
    set_display_height(b, 1); /* VD=01 → h=512 */
    set_cc(b, 0x00);
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    for (uint32_t off = 0; off < GVRAM_SIZE; off++)
        assert(b->gvram[off] == 0xAA); /* 何も選択していないので不変 */
    assert((x68k_bus_read8(b, 0xE80481) & 0x02u) != 0); /* 1回目: 1（実行中） */
    assert((x68k_bus_read8(b, 0xE80481) & 0x02u) == 0);  /* 2回目: 0（完了） */

    /* T10: 読み戻しがbyteでもwordでも正しい。電脳倶楽部Vol.37のブートローダと同じ
     * 「トリガ後まず1になったことを確認してから0を待つ」2段階ポーリング相当
     * （$0543C4: bit1==1を待つ／$0543D0: bit1==0を待つ）。 */
    set_cc(b, 0x0F);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    assert((x68k_bus_read8(b, 0xE80481) & 0x02u) != 0);  /* $0543C4相当: 1回目は1 */
    assert((x68k_bus_read16(b, 0xE80480) & 0x0002u) == 0); /* $0543D0相当: 2回目は0 */

    /* T11: bit1|bit3同時書き込みで両方動く。GVRAM/TVRAMを既知パタンにし、
     * R22(src=0,dst=1)・CP=$Fでラスタコピーも起きることを確認する */
    fill_gvram(b, 0xAA);
    memset(b->tvram, 0x00, TVRAM_SIZE);
    for (int p = 0; p < 4; p++)
        b->tvram[(uint32_t)p * 0x20000u] = 0x55u; /* 各プレーンのラスタ0先頭バイトに目印 */
    set_reg(b, 22, 0x0001); /* R22: src=0(高バイト), dst=1(低バイト) */
    x68k_bus_write16(b, 0xE80480, 0x000A); /* bit1|bit3 */
    for (uint32_t off = 0; off < GVRAM_SIZE; off++)
        assert(b->gvram[off] == 0x00); /* bit1: GVRAMクリア成立（同期即時） */
    for (int p = 0; p < 4; p++)
        assert(b->tvram[(uint32_t)p * 0x20000u + 512u] == 0x55u); /* bit3: ラスタコピー成立 */
    {
        uint8_t v1 = x68k_bus_read8(b, 0xE80481);
        assert((v1 & 0x02u) != 0); /* bit1: 1回目の読み出しは1（保留） */
        assert((v1 & 0x08u) != 0); /* bit3: レベル保持（現行仕様） */
        uint8_t v2 = x68k_bus_read8(b, 0xE80481);
        assert((v2 & 0x02u) == 0); /* bit1: 2回目は0（完了） */
        assert((v2 & 0x08u) != 0); /* bit3: 引き続き保持 */
    }

    /* T12: bit1=0の書き込みは無害（保留状態も立たない） */
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0000);
    for (uint32_t off = 0; off < GVRAM_SIZE; off++)
        assert(b->gvram[off] == 0xAA);
    assert((x68k_bus_read8(b, 0xE80481) & 0x02u) == 0);
    assert((x68k_bus_read8(b, 0xE80481) & 0x02u) == 0); /* 何度読んでも0のまま */

    /* T13: ラッチ済みYスクロールを使う。ラスターモデルでvdisp_startを通過させ
     * y_latched=1にした後、R13をライブで別値に書き換えてもクリア位置はラッチ値に従う */
    set_display_height(b, 0); /* VD=00 → h=256 */
    set_cc(b, 0x01);
    set_reg(b, 13, 0);
    {
        int lines[64];
        set_reg(b, 4, 566);
        set_reg(b, 5, 16);
        set_reg(b, 6, 40);
        set_reg(b, 7, 552);
        x68k_bus_crtc_raster_frame_start(b, 10000000 / 60, 256);
        x68k_bus_crtc_raster_flush(b, lines, 64); /* vdisp_start通過 → y_latched=1, gr_y_zero[0]=0 */
        assert(b->crtc.raster.y_latched == 1);
        set_reg(b, 13, 400); /* ライブ値だけ変更、ラッチは古いまま */
    }
    fill_gvram(b, 0xAA);
    x68k_bus_write16(b, 0xE80480, 0x0002);
    /* ラッチ値(0)に従うなら行0-255が消え、ライブ値(400)なら別の範囲になる */
    for (int row = 0; row < 256; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0x00));
    for (int row = 256; row < 512; row++)
        assert(gvram_row_is(b, 0x00000, row, 1024, 0xAA));

    x68k_bus_destroy(b);
    printf("crtc_rapid_clear_test: OK\n");
    return 0;
}
