| tools-x68k/asmtest/scan_equal_nomedia_test.s -- _B_VERIFY(0x41)のメディア無し
| 挙動確認(Issue #165設計書 §5.3 (f))。FD1(B:, PDA=$91)がメディア無しの状態で
| 実行する想定。A:(FD0)側に置いて実行するのでB:を排出したまま呼べる。
|
| 結果: $0FF000.l = _B_VERIFY戻り値。期待 ST0=NR系(0x48|unit)、ST1/ST2=0。

    .text
    .globl  _start
_start:
    move.w  #0x41, %d0        | _B_VERIFY
    move.w  #0x9170, %d1      | PDA=$91(FD1), flags=$70
    move.l  #0x03000001, %d2  | N=3,C=0,H=0,R=1
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF000
    .word   0xff00              | DOS _EXIT
