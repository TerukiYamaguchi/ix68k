| tools-x68k/asmtest/txgr_priority_test.s -- ビデオコントローラR1 TX/GR優先順位
| の動作検証用使い捨てテスト。
|
| mode0で赤い四角(GVRAM)を描き、その上に重ねて文字列を表示する。
| GETCHARで一時停止する。
|   CP1: デフォルト(R1=$00E4, TX>GR固定) → 文字が赤の上に見えるはず
|   CP2: R1=$08E4 (TX=2,GR=0でGR>TXへ切替) → 文字が赤の下に隠れるはず

    .text
    .globl  _start
_start:
    move.w  #0x10, %d0
    move.w  #0, %d1
    trap    #15
    move.w  #0x90, %d0
    trap    #15

    move.w  #0x07C0, 0xE82002   | palette idx1 = 赤

    lea     0xC300C0, %a0        | gvram (96,96)
    move.w  #95, %d0             | 幅96-1
    move.w  #63, %d1             | 高さ64-1
    move.w  #1, %d2              | color idx1
    move.l  #1856, %d3           | row delta
    jsr     fill_sq

    move.w  #0x23, %d0           | _B_LOCATE col=13,row=7 (pixel x=104,y=112)
    move.w  #13, %d1
    move.w  #7, %d2
    trap    #15

    lea     MSG, %a1
    move.w  #0x21, %d0           | _B_PRINT
    trap    #15

    .word   0xff01                | CP1: デフォルト(TX前面)

    move.w  #0x08E4, 0xE82500    | TX=2,GR=0 (GR前面へ切替)

    .word   0xff01                | CP2: GR前面

    .word   0xff00                | DOS _EXIT

fill_sq:
    move.w  %d1, %d5
.fs_row:
    move.w  %d0, %d4
.fs_col:
    move.w  %d2, (%a0)+
    dbra    %d4, .fs_col
    add.l   %d3, %a0
    dbra    %d5, .fs_row
    rts

MSG:
    .ascii  "HELLO\0"
    .even
