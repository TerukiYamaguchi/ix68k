| tools-x68k/asmtest/wr_del_nomedia.s -- IOCS _B_WRITED(0x49)/_B_READDL(0x4C)
| メディアなし実機検証(Issue #166設計書 §5)。FD1(B:, PDA=$91)を排出した状態で
| 呼び出し、D0にNot Ready(ST0 NR)が返ることを確認する。A:(FD0)から実行する。
|
| 結果レイアウト($0FF000起点、各4バイトD0):
|   +0x00: _B_WRITED PDA=$91(FD1、メディアなし) -> D0
|   +0x04: _B_READDL PDA=$91(FD1、メディアなし) -> D0

    .text
    .globl  _start
_start:
    lea     0x0FF000, %a3

    move.w  #0x49, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    move.w  #0x4C, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    .word   0xff00              | DOS _EXIT
