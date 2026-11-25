| tools-x68k/asmtest/crtcras_demo.s -- IOCS 0x6D _CRTCRAS 可視化サンプル (Issue #56)
|
| ラスタ割り込みで、指定したラスタ番号(d1.w)のY座標に水平マーカーバーを描画する。
| d1.wを変えるとマーカーの縦位置が変わることを、256x256(mode2)と512x512(mode0)の
| 両方で確認する(マーカーY座標は0-255に収め、両モードの可視域に必ず入るようにする)。
| GETCHAR($ff01)でチェックポイント一時停止。実CRTCビーム位置との厳密な同期は
| 保証しない近似実装(docs/iocs_support.md 0x6D参照)。

    .equ    GVRAM_BASE, 0xC00000
    .equ    ROWBYTES,   2048
    .equ    MARK_W,     128
    .equ    MARK_H,     4

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

    | RASTER_DEMO raster: 既存の設定を解除→画面クリア→raster行にマーカーが
    | 出る設定で再インストール→PAUSE
    .macro  RASTER_DEMO raster
    move.l  #0, %a1
    move.w  #0x6d, %d0
    trap    #15
    jsr     clr_top
    move.w  #\raster, cr_raster
    lea     cr_handler, %a1
    move.w  #\raster, %d1
    move.w  #0x6d, %d0
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
    RASTER_DEMO 40                  | チェックポイント: マーカーが上寄り(Y=40)
    RASTER_DEMO 200                 | チェックポイント: マーカーが下寄り(Y=200)

    | ===== mode 0: 実画面1024/表示512x512 =====
    CRTMOD_INIT 0
    RASTER_DEMO 40                  | チェックポイント: 同じraster値でも512モードでは上端寄り
    RASTER_DEMO 200

    move.l  #0, %a1
    move.w  #0x6d, %d0
    trap    #15

    .word   0xff00                  | DOS _EXIT

| clr_top: GVRAM上位512ラスタ(実画面1024幅x512行)をクリアする
| (256/512どちらの可視域も十分カバーできる範囲)
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

| ラスタ割り込みハンドラ: cr_raster行目に幅MARK_W高さMARK_H(緑,idx2)の
| 水平マーカーバーを描画する。
| (a1がCPU例外ベクタ表へ直接入るため、生のISRとしてRTEで終わること)
cr_handler:
    movem.l %d0-%d3/%a0, -(%sp)
    move.w  cr_raster, %d1
    mulu.w  #ROWBYTES, %d1          | mulu.wはd1上位バイトのゴミに関係なく
                                     | d1.wを読み32bitクリーンな結果を書く
    lea     GVRAM_BASE, %a0
    add.l   %d1, %a0
    move.w  #MARK_W-1, %d0
    move.w  #MARK_H-1, %d1
    move.w  #2, %d2
    move.l  #(ROWBYTES-MARK_W*2), %d3
    jsr     fill_sq
    movem.l (%sp)+, %d0-%d3/%a0
    rte

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

    .data
cr_raster:
    .word   0
