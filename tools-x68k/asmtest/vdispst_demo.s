| tools-x68k/asmtest/vdispst_demo.s -- IOCS 0x6C _VDISPST 可視化サンプル (Issue #56)
|
| V-DISP割り込みで全画面の背景色を1フレーム毎に赤/青と交互に切り替える。
| GVRAMは全面idx1で塗りつぶし固定、切り替わるのはパレットレジスタ本体
| (idx1の中身)。フリッカーフリーで毎フレーム確実に切り替わることを確認する。
| GETCHAR($ff01)でチェックポイント一時停止。

    .equ    GVRAM_BASE, 0xC00000
    .equ    ROWBYTES,   2048
    .equ    PAL1,       0xE82002
    .equ    COL_RED,    0x07C0
    .equ    COL_BLUE,   0x003E

    .macro  CRTMOD_INIT mode
    move.w  #0x10, %d0
    move.w  #\mode, %d1
    trap    #15
    move.w  #0x90, %d0
    trap    #15
    move.w  #0xE4, 0xE82500
    move.w  #0, 0xE80018
    move.w  #0, 0xE8001A
    .endm

    .macro  PAUSE
    .word   0xff01
    .endm

    .text
    .globl  _start
_start:
    move.w  #0x23, %d0
    move.w  #0, %d1
    move.w  #23, %d2
    trap    #15

    CRTMOD_INIT 2

    | idx1固定の赤で画面全体(256x256)を塗りつぶす。切り替わるのはパレット本体。
    move.w  #COL_RED, PAL1
    lea     GVRAM_BASE, %a0
    move.w  #256-1, %d1
.vd_row:
    move.w  #256-1, %d0
.vd_col:
    move.w  #1, (%a0)+
    dbra    %d0, .vd_col
    add.l   #(ROWBYTES-256*2), %a0
    dbra    %d1, .vd_row
    PAUSE                            | チェックポイント1: 割り込み未設置(赤固定)

    | V-DISP割り込み設定: 垂直帰線期間(hb=0)、カウンタ1(毎回発火)
    lea     vd_handler, %a1
    move.w  #0x0001, %d1
    move.w  #0x6c, %d0
    trap    #15
    PAUSE                            | チェックポイント2: 点滅中(赤青どちらかの瞬間値)
    PAUSE                            | チェックポイント3: もう一度(前回と違う色なら点滅の証拠)

    move.l  #0, %a1
    move.w  #0x6c, %d0
    trap    #15
    PAUSE                            | チェックポイント4: 禁止後は色固定に戻る

    .word   0xff00

| V-DISP割り込みハンドラ: パレットidx1を赤/青でトグルするだけ。
| (a1がCPU例外ベクタ表へ直接入るため、生のISRとしてRTEで終わること)
vd_handler:
    move.l  %d0, -(%sp)
    move.w  vd_color, %d0
    eor.w   #(COL_RED ^ COL_BLUE), %d0
    move.w  %d0, vd_color
    move.w  %d0, PAL1
    move.l  (%sp)+, %d0
    rte

    .data
vd_color:
    .word   COL_RED
