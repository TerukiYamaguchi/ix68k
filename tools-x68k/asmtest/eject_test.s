| tools-x68k/asmtest/eject_test.s -- Issue #160 Phase 4動作検証
| _B_EJECT(0x4F)本体のROM委譲を検証する。FD1(B:, PDA=$91)に対し、
| ロック無しで_B_EJECTを発行し実際に排出されるか、非FDD PDA($80)と
| 未搭載ドライブ(PDA=$93)がスタブどおりD0=-1を返すかを確認する。
|
| 結果レイアウト($0FF000起点、各4バイトD0):
|   +0x00: mode=8 初期確認 (期待:1、未排出)
|   +0x04: _B_EJECT(0x4F) PDA=$91 -> D0
|   +0x08: mode=8 排出検査 (期待:-1、実際に排出された)
|   +0x0C: _B_EJECT(0x4F) PDA=$80(SASI, 未接続) -> D0(期待:-1)
|   +0x10: _B_EJECT(0x4F) PDA=$93(未搭載ドライブ) -> D0(期待:-1)

    .text
    .globl _start
_start:
    lea     0x0FF000, %a3

    | mode=8 初期確認
    move.w  #0x4E, %d0
    move.w  #0x9100, %d1
    move.w  #8, %d2
    trap    #15
    move.l  %d0, (%a3)+

    | _B_EJECT PDA=$91
    move.w  #0x4F, %d0
    move.w  #0x9100, %d1
    trap    #15
    move.l  %d0, (%a3)+

    | mode=8 排出検査
    move.w  #0x4E, %d0
    move.w  #0x9100, %d1
    move.w  #8, %d2
    trap    #15
    move.l  %d0, (%a3)+

    | _B_EJECT PDA=$80 (SASI, 未接続)
    move.w  #0x4F, %d0
    move.w  #0x8000, %d1
    trap    #15
    move.l  %d0, (%a3)+

    | _B_EJECT PDA=$93 (未搭載ドライブ)
    move.w  #0x4F, %d0
    move.w  #0x9300, %d1
    trap    #15
    move.l  %d0, (%a3)+

    .word   0xff00              | DOS _EXIT
