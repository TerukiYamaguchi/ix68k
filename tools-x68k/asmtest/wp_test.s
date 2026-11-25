| tools-x68k/asmtest/wp_test.s -- ライトプロテクト(WP)動作検証(Issue #167設計書 §5)
| FD1(B:, PDA=$91) がライトプロテクト中に _B_DRVSNS/_B_WRITE/_B_FORMAT が
| 正しくWP/NWを返すことを確認する。結果を固定RAMアドレスへ並べて保存する。
|
| 結果レイアウト($0FF000起点、各4バイトD0):
|   +0x00: (1) _B_DRVSNS(0x44) PDA=$91 -> D0 (bit30=WPが立つことを期待)
|   +0x04: (2) _B_WRITE(0x45) C10H0R1 -> D0 (AT|NW = $40020000台を期待)
|   +0x08: (3) _B_FORMAT(0x4D) C10H0 N=3,SC=8 -> D0 (bit17=NWを期待)
|   +0x0C: (4) _B_READ(0x46) C10H0R1 -> D0 (0を期待、読み出し自体はWPの影響を受けない)

    .text
    .globl  _start
_start:
    lea     0x0FF000, %a3       | 結果ポインタ

    | ---- (1) _B_DRVSNS(0x44) PDA=$91 ----
    move.w  #0x44, %d0
    move.w  #0x9100, %d1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (2) _B_WRITE(0x45) C10H0R1 ----
    lea     0x0F0000, %a2
    move.w  #1023, %d5
fill_p1:
    move.w  %d5, %d4
    move.b  %d4, (%a2)+
    dbra    %d5, fill_p1

    move.w  #0x45, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (3) _B_FORMAT(0x4D) C10H0 N=3,SC=8,fill=$E5 ----
    move.w  #0x4D, %d0
    move.w  #0x9100, %d1
    move.l  #0x030A00E5, %d2
    move.l  #32, %d3
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (4) _B_READ(0x46) C10H0R1(WPの影響を受けないことの確認) ----
    lea     0x0F1000, %a2
    move.w  #1023, %d5
clear_r1:
    clr.b   (%a2)+
    dbra    %d5, clear_r1
    move.w  #0x46, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    .word   0xff00              | DOS _EXIT
