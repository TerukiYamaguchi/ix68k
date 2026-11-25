| tools-x68k/asmtest/timerdst_demo.s -- IOCS 0x6B _TIMERDST 可視化サンプル (Issue #56)
|
| Timer D割り込みで正方形を赤/緑に交互トグルし続ける点滅表示。
| GETCHAR($ff01)でチェックポイント一時停止。PAUSE直前がscreenshot撮影ポイント。
| GVRAM(x,y)ワードアドレス = 0xC00000 + y*2048 + x*2 (実画面1024の16色1ページモード、
| renderer.c x68k_render()のstride*2バイト/pixelと一致、crtmod_test.sの
| rowdelta=1984(=2048-32*2)とも整合)。

    .equ    GVRAM_BASE, 0xC00000
    .equ    ROWBYTES,   2048
    .equ    SQ_Y,       64
    .equ    SQ_X,       112
    .equ    SQ_SIZE,    32

    .macro  CRTMOD_INIT mode
    move.w  #0x10, %d0
    move.w  #\mode, %d1
    trap    #15
    move.w  #0x90, %d0
    trap    #15
    move.w  #0xE4, 0xE82500
    move.w  #0, 0xE80018
    move.w  #0, 0xE8001A
    move.w  #0x07C0, 0xE82002
    move.w  #0xF800, 0xE82004
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
    trap    #15                    | カーソルを描画域外へ

    CRTMOD_INIT 2

    | 初期状態: 正方形を赤(idx1)で描画
    lea     (GVRAM_BASE + SQ_Y*ROWBYTES + SQ_X*2), %a0
    move.w  #SQ_SIZE-1, %d0
    move.w  #SQ_SIZE-1, %d1
    move.w  #1, %d2
    move.l  #(ROWBYTES-SQ_SIZE*2), %d3
    jsr     fill_sq
    PAUSE                           | チェックポイント1: 割り込み未設置(赤固定)

    | Timer D割り込み設定: 単位時間50.0us(7), カウンタ0(=256) -> 12.8ms周期
    lea     td_handler, %a1
    move.w  #0x0700, %d1
    move.w  #0x6b, %d0
    trap    #15
    PAUSE                           | チェックポイント2: 点滅中(赤緑どちらかの瞬間値)
    PAUSE                           | チェックポイント3: もう一度(前回と違う色なら点滅の証拠)

    | 割り込み禁止に戻す
    move.l  #0, %a1
    move.w  #0x6b, %d0
    trap    #15
    PAUSE                           | チェックポイント4: 禁止後は色固定に戻る

    .word   0xff00                  | DOS _EXIT

| fill_sq: a0=開始アドレス d0=幅-1 d1=高さ-1 d2.w=書き込む値 d3.l=行送りバイト数
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

| Timer D割り込みハンドラ: 30回に1回、正方形を赤(1)/緑(2)にトグル
| (IOCSがa1をCPU例外ベクタ表へ直接格納するため、本ルーチンは生の割り込み
|  ハンドラとして呼ばれる。使用レジスタは全て退避しRTEで終わること)
td_handler:
    movem.l %d0-%d5/%a0, -(%sp)
    addq.w  #1, td_subcount
    cmp.w   #30, td_subcount
    blt     td_done
    clr.w   td_subcount
    eor.w   #3, td_color            | 1 XOR 3=2, 2 XOR 3=1 でトグル
    lea     (GVRAM_BASE + SQ_Y*ROWBYTES + SQ_X*2), %a0
    move.w  #SQ_SIZE-1, %d0
    move.w  #SQ_SIZE-1, %d1
    move.w  td_color, %d2
    move.l  #(ROWBYTES-SQ_SIZE*2), %d3
    jsr     fill_sq
td_done:
    movem.l (%sp)+, %d0-%d5/%a0
    rte

    .data
td_subcount:
    .word   0
td_color:
    .word   1
