| tools-x68k/asmtest/fntget_test.s -- _FNTGET(0x19) ROM委譲の動作検証
|
| 'A'(0x41)の8x16/16x16 ANKパターンを取得し、結果を0xF8000へ書き出す
| （メインRAMは0x000000-0x0FFFFFの1MBのみ。プログラム自体と衝突しない
| 末尾寄りの領域を使う）。
| 0xF8000: (0,a1)=Xドット数 (2,a1)=Yドット数 (4,a1..)=パターンデータ
| 0xF8010: D0返値（パターン格納元アドレス）

    .text
    .globl _start
_start:
    move.l  #0x00000041, %d1  | d1 upper16=0(8x16/16x16 ANK) lower16=0x0041('A')
    lea     0xF8000, %a1
    move.w  #0x19, %d0
    trap    #15
    move.l  %d0, 0xF8010

    .word   0xff00            | DOS _EXIT
