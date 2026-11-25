| tools-x68k/asmtest/datebcd_test.s -- _DATEBCD/_TIMEBCD(0x50/0x52) ROM委譲の動作検証
|
| 2026/07/13(月曜・閏年カウンタ=year%4=2) と 12:34:56 を変換し、
| 手計算した期待値と実機出力を比較する。
| 期待値: DATEBCD -> 0x21260713 (leap=2,dow=1(月),年BCD26,月BCD07,日BCD13)
|         TIMEBCD -> 0x01123456 (flag=1,時BCD12,分BCD34,秒BCD56)
| 0xF8100: DATEBCD結果(long)  0xF8104: TIMEBCD結果(long)

    .text
    .globl _start
_start:
    | DATEBCD(0x50): 2026/07/13 バイナリ -> BCD
    move.l  #0x07EA070D, %d1
    move.w  #0x50, %d0
    trap    #15
    move.l  %d0, 0xF8100

    | TIMEBCD(0x52): 12:34:56 バイナリ -> BCD
    move.l  #0x000C2238, %d1
    move.w  #0x52, %d0
    trap    #15
    move.l  %d0, 0xF8104

    .word   0xff00            | DOS _EXIT
