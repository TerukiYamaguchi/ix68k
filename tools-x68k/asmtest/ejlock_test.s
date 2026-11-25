| tools-x68k/asmtest/drvchk_eject_lock_test.s -- Issue #160 Phase 4実機検証
| _B_DRVCHK(0x4E) mode=1(イジェクト)がROM内部のロックシャドウ($9E2+drv由来)を
| 尊重するかを検証する。FD1(B:, PDA=$91)を対象に:
|   mode=2(ロックON, 推定) -> mode=1(イジェクト試行) -> mode=8(排出検査)
|   -> mode=3(ロックOFF, 推定) -> mode=1(イジェクト試行) -> mode=8(排出検査)
| 期待: ロック中はmode=8が1(未排出)のまま、ロック解除後はmode=8が-1(排出済み)になる。
|
| 結果レイアウト($0FF000起点、各4バイトD0):
|   +0x00: mode=8 初期確認 (期待:1、まだ排出していない)
|   +0x04: mode=2 (ロックON)
|   +0x08: mode=1 イジェクト試行(ロック中) -> D0(ステータス、意味は薄い)
|   +0x0C: mode=8 排出検査(期待:1、ロックによりブロックされ未排出のまま)
|   +0x10: mode=3 (ロックOFF)
|   +0x14: mode=1 イジェクト試行(ロック解除後) -> D0
|   +0x18: mode=8 排出検査(期待:-1、実際に排出された)

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

    | mode=2 ロックON
    move.w  #0x4E, %d0
    move.w  #0x9100, %d1
    move.w  #2, %d2
    trap    #15
    move.l  %d0, (%a3)+

    | mode=1 イジェクト試行(ロック中のはず)
    move.w  #0x4E, %d0
    move.w  #0x9100, %d1
    move.w  #1, %d2
    trap    #15
    move.l  %d0, (%a3)+

    | mode=8 排出検査
    move.w  #0x4E, %d0
    move.w  #0x9100, %d1
    move.w  #8, %d2
    trap    #15
    move.l  %d0, (%a3)+

    | mode=3 ロックOFF
    move.w  #0x4E, %d0
    move.w  #0x9100, %d1
    move.w  #3, %d2
    trap    #15
    move.l  %d0, (%a3)+

    | mode=1 イジェクト試行(ロック解除後)
    move.w  #0x4E, %d0
    move.w  #0x9100, %d1
    move.w  #1, %d2
    trap    #15
    move.l  %d0, (%a3)+

    | mode=8 排出検査
    move.w  #0x4E, %d0
    move.w  #0x9100, %d1
    move.w  #8, %d2
    trap    #15
    move.l  %d0, (%a3)+

    .word   0xff00              | DOS _EXIT
