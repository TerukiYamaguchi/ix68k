| tools-x68k/joyget_test/joyget_test.s -- IOCS 0x3B _JOYGET でジョイスティックを読むサンプル
|
| ジョイスティック#1/#2 の十字キー・A/B ボタン・SELECT・START を画面に表示する。
| ESC キーで終了する。
|
| _JOYGET (d0=$3B, d1.w=0/1) は PPI ポート A/B の生値を d0.b に返す（アクティブロー）。
|   bit0 上 / bit1 下 / bit2 左 / bit3 右 / bit5 トリガA / bit6 トリガB
| 4方向+2ボタンのポートで SELECT/START を表す慣例に従い、
|   SELECT = 上下の同時押し、START = 左右の同時押し として判定する。
| SELECT/START 押下中は上下/左右も押下扱いで表示される（慣例どおり）。

    .equ    IOCS_B_KEYINP,  0x00
    .equ    IOCS_B_KEYSNS,  0x01
    .equ    IOCS_B_PRINT,   0x21
    .equ    IOCS_B_LOCATE,  0x23
    .equ    IOCS_B_CLR_ST,  0x2A
    .equ    IOCS_JOYGET,    0x3B

    .equ    JOY_UP,         0x01
    .equ    JOY_DOWN,       0x02
    .equ    JOY_LEFT,       0x04
    .equ    JOY_RIGHT,      0x08
    .equ    JOY_TRG_A,      0x20
    .equ    JOY_TRG_B,      0x40
    .equ    JOY_SELECT,     JOY_UP + JOY_DOWN
    .equ    JOY_START,      JOY_LEFT + JOY_RIGHT

    .equ    KEY_ESC,        0x1B
    .equ    CLR_ST_ALL,     2           | _B_CLR_ST: 画面全体消去
    .equ    JOY_PORTS,      2
    .equ    JOY_RELEASED,   0xFF
    .equ    ROW_FIRST,      3           | 1台目の表示行
    .equ    ROW_STEP,       2           | 1台ごとの行間
    .equ    BUTTON_COUNT,   8
    .equ    NAME_STRIDE,    7           | 6文字+NUL

    .macro  IOCS no
    move.w  #\no, %d0
    trap    #15
    .endm

    .text
    .globl  _start
_start:
    move.w  #CLR_ST_ALL, %d1
    IOCS    IOCS_B_CLR_ST
    moveq   #0, %d1
    moveq   #0, %d2
    IOCS    IOCS_B_LOCATE
    lea     title_msg, %a1
    IOCS    IOCS_B_PRINT

    | 前回値を「未接続・無操作」で初期化（初回は必ず描画させるため 0 にする）
    clr.b   prev_val
    clr.b   prev_val+1

main_loop:
    moveq   #0, %d7                  | d7 = ジョイスティック番号
joy_loop:
    move.w  %d7, %d1
    IOCS    IOCS_JOYGET              | d0.b = ポート値
    and.w   #0xFF, %d0
    lea     prev_val, %a0
    cmp.b   (%a0,%d7.w), %d0
    beq     joy_next                 | 変化なしなら再描画しない
    move.b  %d0, (%a0,%d7.w)
    move.w  %d0, %d6                 | d6 = 現在値（描画中に d0 を壊すため退避）
    bsr     draw_joy
joy_next:
    addq.w  #1, %d7
    cmp.w   #JOY_PORTS, %d7
    blt     joy_loop

    | ESC で終了（キー入力があるときだけ読む）
    IOCS    IOCS_B_KEYSNS
    tst.l   %d0
    beq     main_loop
    IOCS    IOCS_B_KEYINP
    cmp.b   #KEY_ESC, %d0
    bne     main_loop

    moveq   #ROW_FIRST+ROW_STEP*JOY_PORTS, %d2
    moveq   #0, %d1
    IOCS    IOCS_B_LOCATE
    .word   0xff00                   | DOS _EXIT

| draw_joy: d7=番号, d6=ポート値を1行に描画する。
draw_joy:
    moveq   #0, %d1
    move.w  %d7, %d2
    mulu.w  #ROW_STEP, %d2
    add.w   #ROW_FIRST, %d2
    IOCS    IOCS_B_LOCATE
    lea     joy_label, %a1           | "JOY#n:" の n を書き換えてから表示
    move.b  %d7, %d0
    add.b   #'1', %d0
    move.b  %d0, joy_label_no
    IOCS    IOCS_B_PRINT

    lea     name_table, %a2
    lea     mask_table, %a3
    moveq   #BUTTON_COUNT-1, %d5
draw_loop:
    move.w  (%a3)+, %d3              | d3 = 判定マスク
    move.w  %d6, %d0
    and.w   %d3, %d0                 | マスクの全ビットが 0 なら押下
    move.l  %a2, %a1                 | 押されている: ボタン名
    tst.w   %d0
    beq     draw_emit
    lea     blank_name, %a1          | 押されていない: 空白で名前を消す
draw_emit:
    IOCS    IOCS_B_PRINT
    add.l   #NAME_STRIDE, %a2
    dbra    %d5, draw_loop
    rts

    .data
title_msg:
    .ascii  "JOYGET_TEST.X Ver.1.0  _JOYGET test  (ESC: quit)\r\n"
    .ascii  "SELECT = UP+DOWN, START = LEFT+RIGHT\r\n"
    .byte   0
joy_label:
    .ascii  "JOY#"
joy_label_no:
    .ascii  "1: "
    .byte   0
    .even
| ボタン名（各 NAME_STRIDE バイト: 5文字+空白+NUL）。該当ビットがすべて 0 のとき表示する。
name_table:
    .asciz  "UP    "
    .asciz  "DOWN  "
    .asciz  "LEFT  "
    .asciz  "RIGHT "
    .asciz  "A     "
    .asciz  "B     "
    .asciz  "SELECT"
    .asciz  "START "
blank_name:
    .asciz  "      "
    .even
| 押下判定マスク（アクティブロー: ビットが全部 0 で押下）
mask_table:
    .word   JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT, JOY_TRG_A, JOY_TRG_B
    .word   JOY_SELECT, JOY_START

    .bss
prev_val:
    .ds.b   JOY_PORTS
