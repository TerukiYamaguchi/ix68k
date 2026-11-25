| tools-x68k/asmtest/hsyncst_demo.s -- IOCS 0x6E _HSYNCST 可視化サンプル (Issue #56)
|
| H-SYNC割り込みは1フレーム中に走査線の本数だけ発火する(0x6Dの_CRTCRASが1回きり
| なのと対照的)。走査線ごとに1行ずつサインカーブ上に点(青,idx3)を打ち、画面全体に
| 正弦波リボンを描く。256x256(mode2)と512x512(mode0)の両方で確認する。
| GETCHAR($ff01)でチェックポイント一時停止。実CRTCビーム位置との厳密な同期は
| 保証しない近似実装(docs/iocs_support.md 0x6E参照)。

    .equ    GVRAM_BASE, 0xC00000
    .equ    ROWBYTES,   2048
    .equ    CENTER_X,   112

    .macro  CRTMOD_INIT mode
    move.w  #0x10, %d0
    move.w  #\mode, %d1
    trap    #15
    move.w  #0x90, %d0
    trap    #15
    move.w  #0xE4, 0xE82500
    move.w  #0, 0xE80018
    move.w  #0, 0xE8001A
    move.w  #0x003E, 0xE82006
    .endm

    .macro  PAUSE
    .word   0xff01
    .endm

    | HSYNC_DEMO height_mask: 既存設定解除→画面クリア→行カウンタ初期化→
    | height_mask設定→インストール→PAUSE
    .macro  HSYNC_DEMO height_mask
    move.l  #0, %a1
    move.w  #0x6e, %d0
    trap    #15
    jsr     clr_top
    move.w  #0, hs_row
    move.w  #\height_mask, hs_height_mask
    lea     hs_handler, %a1
    move.w  #0x6e, %d0
    trap    #15
    PAUSE
    .endm

    .text
    .globl  _start
_start:
    move.w  #0x23, %d0
    move.w  #0, %d1
    move.w  #23, %d2
    trap    #15

    | ===== mode 2: 実画面1024/表示256x256 =====
    CRTMOD_INIT 2
    HSYNC_DEMO 255                  | チェックポイント: 256行分の正弦波リボン

    | ===== mode 0: 実画面1024/表示512x512 =====
    CRTMOD_INIT 0
    HSYNC_DEMO 511                  | チェックポイント: 512行分の正弦波リボン

    move.l  #0, %a1
    move.w  #0x6e, %d0
    trap    #15

    .word   0xff00                  | DOS _EXIT

| clr_top: GVRAM上位512ラスタ(実画面1024幅x512行)をクリアする
clr_top:
    lea     GVRAM_BASE, %a0
    move.w  #512-1, %d1
.ct_row:
    move.w  #1024-1, %d0
.ct_col:
    clr.w   (%a0)+
    dbra    %d0, .ct_col
    dbra    %d1, .ct_row
    rts

| H-SYNC割り込みハンドラ: hs_row行目にサインテーブルのオフセットで4ドット幅の
| セグメント(青,idx3)を描画し、hs_rowをhs_height_maskで折り返しつつ+1する。
| (a1がCPU例外ベクタ表へ直接入るため、生のISRとしてRTEで終わること)
hs_handler:
    movem.l %d0-%d3/%a0, -(%sp)
    move.w  hs_row, %d1
    move.w  %d1, %d3
    and.w   #31, %d3
    add.w   %d3, %d3                | テーブルはword単位なのでバイトオフセットは*2
    lea     sine_table, %a0
    move.w  0(%a0,%d3.w), %d0       | d0.w = サインオフセット(-38..38)
    ext.l   %d0                     | 32bitへ符号拡張(上位ワードのゴミを除去)
    add.l   #CENTER_X, %d0          | d0.l = X座標(74..150、常に正)
    add.l   %d0, %d0                | d0.l = Xバイトオフセット
    move.w  %d1, %d2                | mulu.wはd2.wだけ読み32bit結果で全体を上書き
    mulu.w  #ROWBYTES, %d2          | d2.l = 行バイトオフセット
    lea     GVRAM_BASE, %a0
    add.l   %d2, %a0
    add.l   %d0, %a0
    move.w  #3, (%a0)+
    move.w  #3, (%a0)+
    move.w  #3, (%a0)+
    move.w  #3, (%a0)+
    addq.w  #1, %d1
    and.w   hs_height_mask, %d1
    move.w  %d1, hs_row
    movem.l (%sp)+, %d0-%d3/%a0
    rte

    .data
hs_row:
    .word   0
hs_height_mask:
    .word   255
sine_table:
    .word   0, 7, 15, 21, 27, 32, 35, 37, 38, 37, 35, 32, 27, 21, 15, 7
    .word   0, -7, -15, -21, -27, -32, -35, -37, -38, -37, -35, -32, -27, -21, -15, -7
