/*
 * 実画面1024×1024モードの GVRAM サンプリング位置の回帰テスト。
 *
 * 1024ドットモードの GVRAM は「1ページで2Mバイト」を占め、CPU から見たアドレスは
 * $C00000 + (y*1024 + x)*2 のリニア1面になる（『Inside X68000』3-2-2節）。
 * 画面4象限へどの VRAM バンクを出すかはビデオコントローラ R1 ($E82500) 下位バイトの
 * GP0-GP3 が決める（同3-2-6節。標準値 $E4 で象限とバンクが1対1、$D8 で右上と左下が
 * 入れ替わり、$00 では全象限が左上バンクと同じ内容になる）。
 */
#include "render/renderer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GVRAM_BYTES   0x200000
#define R1_STANDARD   0xE4 /* GP3=11 GP2=10 GP1=01 GP0=00 */
#define R1_SWAP_RT_LB 0xD8 /* GP3=11 GP2=01 GP1=10 GP0=00 */
#define R1_ALL_BANK0  0x00

/* 各バンク内の同じ相対座標(8,8)に、バンクごとに違う色コードを置く */
#define DOT_X      8
#define DOT_Y      8
#define CODE_BANK0 1
#define CODE_BANK1 2
#define CODE_BANK2 3
#define CODE_BANK3 4
#define CODE_DECOY 5 /* 512ストライドで読むと拾えてしまう位置 */

static uint8_t *gvram;

/* リニア面の (x, y) に色コードを置く */
static void put_dot(unsigned x, unsigned y, uint16_t code) {
    size_t off = (size_t)(y * 1024u + x) * 2u;
    gvram[off] = (uint8_t)(code >> 8);
    gvram[off + 1] = (uint8_t)(code & 0xFF);
}

/* 画面座標 (x, y) の色コードを1ドットだけ取り出す（スクロールで x を原点へ寄せる） */
static uint16_t sample(int page_sel, int x, int y) {
    gvram_page_t page;
    uint16_t codes[4 * 1];

    memset(&page, 0, sizeof(page));
    page.mask = 0x0F; /* 16色 */
    page.page_sel = page_sel;
    page.sx = x;
    page.sy = 0;

    x68k_sample_gvram_codes_line(gvram, codes, 1, &page, 1, 1, y);
    return codes[0];
}

/* 標準設定では、リニア面に置いたドットがそのままの座標で読める */
static void test_linear_layout(void) {
    assert(sample(R1_STANDARD, DOT_X, DOT_Y) == CODE_BANK0);
    assert(sample(R1_STANDARD, 512 + DOT_X, DOT_Y) == CODE_BANK1);
    assert(sample(R1_STANDARD, DOT_X, 512 + DOT_Y) == CODE_BANK2);
    assert(sample(R1_STANDARD, 512 + DOT_X, 512 + DOT_Y) == CODE_BANK3);
}

/*
 * 512ワードストライドで読んでいた頃は、画面 (8,8) がリニア面の (8,4) を指していた。
 * そこに置いた囮を拾わないことで、過去の不具合の再発を検出する。
 */
static void test_not_512_stride(void) {
    assert(sample(R1_STANDARD, DOT_X, DOT_Y) != CODE_DECOY);
}

/* GP1 と GP2 を入れ替えると、右上象限と左下象限の内容がそっくり入れ替わる */
static void test_bank_swap(void) {
    assert(sample(R1_SWAP_RT_LB, 512 + DOT_X, DOT_Y) == CODE_BANK2);
    assert(sample(R1_SWAP_RT_LB, DOT_X, 512 + DOT_Y) == CODE_BANK1);
    assert(sample(R1_SWAP_RT_LB, DOT_X, DOT_Y) == CODE_BANK0);
    assert(sample(R1_SWAP_RT_LB, 512 + DOT_X, 512 + DOT_Y) == CODE_BANK3);
}

/* 全 GP を 0 にすると、どの象限も左上バンクと同じ内容になる */
static void test_all_bank0(void) {
    assert(sample(R1_ALL_BANK0, DOT_X, DOT_Y) == CODE_BANK0);
    assert(sample(R1_ALL_BANK0, 512 + DOT_X, DOT_Y) == CODE_BANK0);
    assert(sample(R1_ALL_BANK0, DOT_X, 512 + DOT_Y) == CODE_BANK0);
    assert(sample(R1_ALL_BANK0, 512 + DOT_X, 512 + DOT_Y) == CODE_BANK0);
}

int main(void) {
    gvram = calloc(1, GVRAM_BYTES);
    assert(gvram);

    put_dot(DOT_X, DOT_Y, CODE_BANK0);
    put_dot(512 + DOT_X, DOT_Y, CODE_BANK1);
    put_dot(DOT_X, 512 + DOT_Y, CODE_BANK2);
    put_dot(512 + DOT_X, 512 + DOT_Y, CODE_BANK3);
    put_dot(DOT_X, DOT_Y / 2, CODE_DECOY);

    test_linear_layout();
    test_not_512_stride();
    test_bank_swap();
    test_all_bank0();

    free(gvram);
    printf("gvram_1024_test: OK\n");
    return 0;
}
