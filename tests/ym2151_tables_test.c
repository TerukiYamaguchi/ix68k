/*
 * ym2151_tables.h の単体テスト。
 * XEiJ xeij/YM2151.java 240-440行（ymfm由来）から転記した値と一致することを
 * spot-checkする。合成ロジック(ym2151.c)には未配線のため挙動は変化しない。
 */
#include "audio/ym2151_tables.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    /* s_sin_table: 先頭・末尾 */
    assert(s_sin_table[0] == 0x859u);
    assert(s_sin_table[255] == 0x000u);

    /* s_power_table: X(0x3fa)=(0x3fa|0x400)<<2=0x1FE8, X(0x000)=(0x400)<<2=0x1000 */
    assert(s_power_table[0] == 0x1FE8u);
    assert(s_power_table[255] == 0x1000u);

    /* s_algorithm_ops: ALGORITHM(op2in,op3in,op4in,op1out,op2out,op3out) =
     * op2in | op3in<<1 | op4in<<4 | op1out<<7 | op2out<<8 | op3out<<9 */
    assert(s_algorithm_ops[0] == 0x035u);  /* ALGORITHM(1,2,3,0,0,0) */
    assert(s_algorithm_ops[1] == 0x03Au);  /* ALGORITHM(0,5,3,0,0,0) */
    assert(s_algorithm_ops[2] == 0x064u);  /* ALGORITHM(0,2,6,0,0,0) */
    assert(s_algorithm_ops[3] == 0x071u);  /* ALGORITHM(1,0,7,0,0,0) */
    assert(s_algorithm_ops[4] == 0x131u);  /* ALGORITHM(1,0,3,0,1,0) */
    assert(s_algorithm_ops[5] == 0x313u);  /* ALGORITHM(1,1,1,0,1,1) */
    assert(s_algorithm_ops[6] == 0x301u);  /* ALGORITHM(1,0,0,0,1,1) */
    assert(s_algorithm_ops[7] == 0x380u);  /* ALGORITHM(0,0,0,1,1,1) */
    /* CON=0-7がOPMの全域。8-11はOPN系との共通テーブル拡張分でOPM未使用だが、
     * テーブル自体はXEiJ原文通り12要素で保持している（インデックス範囲確認）。 */
    assert(s_algorithm_ops[8] == s_algorithm_ops[0]);  /* コメント通り0と同一 */
    assert(s_algorithm_ops[10] == s_algorithm_ops[4]); /* コメント通り4と同一 */

    /* s_detune2_delta: {0, (600*64+50)/100, (781*64+50)/100, (950*64+50)/100} */
    assert(s_detune2_delta[0] == 0);
    assert(s_detune2_delta[1] == 384);
    assert(s_detune2_delta[2] == 500);
    assert(s_detune2_delta[3] == 608);

    /* 他テーブルの先頭・末尾（転記ミス検出用） */
    assert(s_increment_table[0] == 0x00000000u);
    assert(s_increment_table[63] == 0x88888888u);
    assert(s_detune_adjustment[0] == 0);
    assert(s_detune_adjustment[127] == 22);
    assert(s_phase_step[0] == 41568u);
    assert(s_phase_step[767] == 82976u);
    assert(s_lfo_pm_shifts[0] == 0x77u);
    assert(s_lfo_pm_shifts[63] == 0x01u);

    /* s_fixed_map: OPLIST(o1,o2,o3,o4) = o1 | o2<<8 | o3<<16 | o4<<24 */
    assert(s_fixed_map[0] == (0u | (16u << 8) | (8u << 16) | (24u << 24)));
    assert(s_fixed_map[7] == (7u | (23u << 8) | (15u << 16) | (31u << 24)));

    printf("ym2151_tables_test: OK\n");
    return 0;
}
