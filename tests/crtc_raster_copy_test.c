/*
 * CRTC動作ポート ($E80481) bit3 の水平フロントポーチごとの継続実行を検証する
 * 単体テスト。XEiJ CRTC.java（crtRasterCopyOn/crtDoRasterCopy）が
 * 「bit3は0を書くまで1のまま保持され、各水平フロントポーチでコピーを続ける
 * MPU側スイッチ」と明記していることを根拠に、MPX68K commit f59c07dと同じ
 * 「書き込み時の即時コピー + フロントポーチ継続実行」の二重経路を実装した。
 * MPX68K tools/raster-copy-test/rastercopytest.xdf の目視判定基準
 * （COPY ON中はコピー先がコピー元に追従、OFFで凍結）をそのまま単体テスト化する。
 */
#include "bus/bus_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_reg(x68k_bus_t *b, int reg_num, uint16_t val) {
    b->crtc_regs[reg_num * 2] = (uint8_t)(val >> 8);
    b->crtc_regs[reg_num * 2 + 1] = (uint8_t)(val & 0xFFu);
}

static void set_cc(x68k_bus_t *b, uint8_t mask) {
    b->crtc_regs[0x2Bu] = (uint8_t)(mask & 0x0Fu);
}

/* R04-R07を有効な垂直タイミングにしてラスターモデルをvalid=1にする
 * （crtc_rapid_clear_test.c T13と同じ値）。frame_start()呼び出し前に必要。 */
static void set_valid_vtiming(x68k_bus_t *b) {
    set_reg(b, 4, 566);
    set_reg(b, 5, 16);
    set_reg(b, 6, 40);
    set_reg(b, 7, 552);
}

int main(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* plane0のみ選択、R22: src=block0(offset0)→dst=block1(offset512)。
     * テストディスクと同じく「R22は最初に一度だけ設定し、以後は触らない」。 */
    set_cc(b, 0x01);
    set_reg(b, 22, 0x0001);
    b->tvram[0] = 0xAAu;   /* コピー元（以後、ここだけ書き換えて動かす） */
    b->tvram[512] = 0x00u; /* コピー先（初期値はコピー元と別にしておく） */

    /* COPY ON（書き込み時の即時コピー）。 */
    x68k_bus_write16(b, 0xE80480, 0x0008);
    assert(b->tvram[512] == 0xAAu);
    assert((x68k_bus_read8(b, 0xE80481) & 0x08u) != 0); /* bit3はレベルのまま保持 */

    /* T1: COPY ON中はコピー元だけを書き換えても、ラスタが進めば
     * 水平フロントポーチ実行でコピー先が追従する（fix前は書き込み時の1回きりで
     * 以後追従せず、この時点でdstは0xAAのまま固まっていた）。 */
    b->tvram[0] = 0xBBu;
    set_valid_vtiming(b);
    x68k_bus_crtc_raster_frame_start(b, 10000000 / 60, 256);
    {
        int lines[64];
        x68k_bus_crtc_raster_flush(b, lines, 64);
    }
    assert(b->tvram[512] == 0xBBu);

    /* T2: COPY OFFにすると、以後コピー元を書き換えてもコピー先は凍結する。 */
    x68k_bus_write16(b, 0xE80480, 0x0000);
    assert((x68k_bus_read8(b, 0xE80481) & 0x08u) == 0);
    b->tvram[0] = 0xCCu;
    x68k_bus_crtc_raster_frame_start(b, 10000000 / 60, 256);
    {
        int lines[64];
        x68k_bus_crtc_raster_flush(b, lines, 64);
    }
    assert(b->tvram[512] == 0xBBu); /* OFF中に追従していない（凍結） */
    assert(b->tvram[0] == 0xCCu);   /* コピー元は変わり続けている */

    x68k_bus_destroy(b);
    printf("crtc_raster_copy_test: OK\n");
    return 0;
}
