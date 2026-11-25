| tools-x68k/asmtest/r2_effects_test.s -- Issue #119 R2半透明・特殊プライオリティ 実機検証
|
| mode4(16色4ページ,512x512)で5バンドを縦に並べ、各バンドでGRページに
| 偶数コード(2,左64px)/奇数コード(3,右64px)の帯を描き、R2を切り替えて比較する。
| 領域判定はベースの生パレットコードbit0で決まる設計のため、偶数側は常に
| 無変化の赤(idx2)のはずで、奇数側だけがR2モードに応じて変化する。
|
| バンド構成(x=96..223, 各高さ48, 左64px=偶数code2, 右64px=奇数code3):
|   y=80  : R2=NORMAL              (偶数=奇数=素の赤、基準)
|   y=144 : R2=AH+B/P              (奇数側だけ 赤×テキストパレット0(青) の半透明)
|   y=208 : R2=EXON+H/P+B/P+G/G    (奇数側だけ 赤×page1のcode4(=code|1→idx5緑)の半透明。
|                                    page1のGSビットはOFFのままにして、G/GがGS無視で
|                                    セカンドページを読むことを確認する)
|   y=272 : R2=EXON+H/P+B/P+G/T    (R1でGRをTXより前面に設定。奇数側のうちTX無しの
|                                    左半分はテキストパレット0(青)、TXあり(白)の右半分は
|                                    赤×白の半透明になるはず)
|   y=336 : R2=EXON+B/P (H/P=0)    (特殊プライオリティ。R1でTXをGRより前面に設定した上で
|                                    TXを帯全体に敷く。偶数側はTXに覆われ白のまま、
|                                    奇数側だけGRベースが昇格しTXより前に出て赤になるはず)
|
| GETCHAR ($ff01, エコーあり) でチェックポイント毎に一時停止する。
| MCPのkey(action="press", ...)で1文字送って進める。各PAUSE直前が撮影ポイント。

    .macro MYINIT mode
    move.w  #0x10, %d0
    move.w  #\mode, %d1
    trap    #15
    move.w  #0x90, %d0
    trap    #15
    move.w  #0, 0xE80018
    move.w  #0, 0xE8001A
    move.w  #0, 0xE8001C
    move.w  #0, 0xE8001E
    move.w  #0, 0xE80020
    move.w  #0, 0xE80022
    move.w  #0, 0xE80024
    move.w  #0, 0xE80026
    .endm

    | GRページ帯塗り: addr, 幅(px), 高さ, 色コード
    .macro FILLGR addr,w,h,color
    lea     \addr, %a0
    move.w  #\w-1, %d0
    move.w  #\h-1, %d1
    move.w  #\color, %d2
    move.l  #1024-\w*2, %d3
    jsr     fill_gr
    .endm

    | TVRAM plane0帯塗り(コード1=plane0のみ1): addr, 幅(byte), 高さ
    .macro FILLTV addr,wbytes,h
    lea     \addr, %a0
    move.w  #\wbytes-1, %d0
    move.w  #\h-1, %d1
    move.l  #128-\wbytes, %d3
    jsr     fill_tv
    .endm

    .macro PAUSE
    .word   0xff01
    .endm

    .text
    .globl  _start
_start:
    | カーソルを画面外(col0,row23)へ逃がす
    move.w  #0x23, %d0
    move.w  #0, %d1
    move.w  #23, %d2
    trap    #15

    MYINIT  4

    | パレット: グラフィックidx2=idx3=赤(奇偶で同一にして基準バンドの差分を無くす),
    |          idx5=緑, テキストidx0=青, テキストidx1=白
    move.w  #0x07C0, 0xE82004
    move.w  #0x07C0, 0xE82006
    move.w  #0xF800, 0xE8200A
    move.w  #0x003E, 0xE82200
    move.w  #0xFFFE, 0xE82202

    | ===== バンド1: R2=NORMAL (基準) =====
    move.b  #0xE4, 0xE82501
    move.b  #0x00, 0xE82500
    FILLGR  0xC140C0,64,48,2
    FILLGR  0xC14140,64,48,3
    move.w  #0x0001, 0xE82600
    PAUSE

    | ===== バンド2: R2=AH+B/P =====
    FILLGR  0xC240C0,64,48,2
    FILLGR  0xC24140,64,48,3
    move.w  #0x4401, 0xE82600
    PAUSE

    | ===== バンド3: R2=EXON+H/P+B/P+G/G (page1 GS=OFFのまま) =====
    FILLGR  0xC340C0,64,48,2
    FILLGR  0xC34140,64,48,3
    FILLGR  0xCB40C0,128,48,4
    move.w  #0x1E01, 0xE82600
    PAUSE

    | ===== バンド4: R2=EXON+H/P+B/P+G/T (R1: GR>TX>SP, TON=1) =====
    FILLGR  0xC440C0,64,48,2
    FILLGR  0xC44140,64,48,3
    FILLTV  0xE08818,4,48
    move.b  #0x24, 0xE82500
    move.w  #0x1D21, 0xE82600
    PAUSE

    | ===== バンド5: R2=EXON+B/P (H/P=0、特殊プライオリティ)。R1: TX>GR>SP =====
    FILLGR  0xC540C0,64,48,2
    FILLGR  0xC54140,64,48,3
    FILLTV  0xE0A80C,16,48
    move.b  #0x21, 0xE82500
    move.w  #0x1421, 0xE82600
    PAUSE

    .word   0xff00          | DOS _EXIT

| fill_gr: a0=開始アドレス %d0=幅-1 %d1=高さ-1 %d2.w=色コード %d3.l=行送りバイト数
fill_gr:
    move.w  %d1, %d5
.fg_row:
    move.w  %d0, %d4
.fg_col:
    move.w  %d2, (%a0)+
    dbra    %d4, .fg_col
    add.l   %d3, %a0
    dbra    %d5, .fg_row
    rts

| fill_tv: a0=開始アドレス(plane0) %d0=幅バイト-1 %d1=高さ-1 %d3.l=行送りバイト数
fill_tv:
    move.w  %d1, %d5
.ft_row:
    move.w  %d0, %d4
.ft_col:
    move.b  #0xFF, (%a0)+
    dbra    %d4, .ft_col
    add.l   %d3, %a0
    dbra    %d5, .ft_row
    rts
