| tools-x68k/asmtest/r2_1024dot_test.s -- 1024ドット(16色のみ)でのR2半透明 動作検証
|
| mode4で起動後、VC R0($E82400)を1024ドット(bit2)にする。1024ドットのGVRAMは
| $C00000+(y*1024+x)*2 のリニア面で、象限(左上)はR1標準値$E4でバンク0。
| R0にはあえて65536色指定(bit1-0=3)を重ね、色数指定が無視されて16色として
| 読まれることも確認する（コードの下位4bitをパレットindexとして引く）。
|
| 3バンド(x=96..223, 各高さ48, 左64px/右64px):
|   y=80  : R2=NORMAL             (左code4=右code6、どちらも素の赤)
|   y=144 : R2=AH+B/P=1           (左code2=偶数:赤のまま / 右code3=奇数:赤×青の半透明)
|   y=208 : R2=EXON+H/P+G/T, B/P=0 (R1: GR>TX>SP。B/P=0が有効なのはこの組み合わせだけ。
|                                    左code4=pal4のI=0:赤のまま / 右code6=pal6のI=1:赤×テキストパレット0(青)の半透明)
|
| GETCHAR ($ff01) でチェックポイント毎に一時停止。各PAUSE直前が撮影ポイント。

    .macro PAUSE
    .word   0xff01
    .endm

    | 1024ドット面帯塗り: addr, 幅(px), 高さ, 色コード（行送り2048バイト）
    .macro FILLGR addr,w,h,color
    lea     \addr, %a0
    move.w  #\w-1, %d0
    move.w  #\h-1, %d1
    move.w  #\color, %d2
    jsr     fill_gr
    .endm

    .text
    .globl  _start
_start:
    move.w  #0x23, %d0
    move.w  #0, %d1
    move.w  #23, %d2
    trap    #15

    move.w  #0x10, %d0
    move.w  #4, %d1
    trap    #15
    move.w  #0x90, %d0
    trap    #15

    | パレット: idx2=idx3=idx4=赤(I=0), idx6=赤(I=1), テキストidx0=青
    move.w  #0x07C0, 0xE82004
    move.w  #0x07C0, 0xE82006
    move.w  #0x07C0, 0xE82008
    move.w  #0x07C1, 0xE8200C
    move.w  #0x003E, 0xE82200

    move.w  #0x0007, 0xE82400      | 1024ドット + 65536色指定（16色として扱われるはず）
    move.b  #0xE4, 0xE82501
    move.b  #0x00, 0xE82500

    | バンド1: NORMAL
    FILLGR  0xC280C0,64,48,4
    FILLGR  0xC28140,64,48,6
    move.w  #0x0001, 0xE82600
    PAUSE

    | バンド2: AH+B/P=1
    FILLGR  0xC480C0,64,48,2
    FILLGR  0xC48140,64,48,3
    move.w  #0x4401, 0xE82600
    PAUSE

    | バンド3: EXON+H/P+G/T, B/P=0 (R1: GR>TX>SP)
    move.b  #0x24, 0xE82500
    FILLGR  0xC680C0,64,48,4
    FILLGR  0xC68140,64,48,6
    move.w  #0x1921, 0xE82600
    PAUSE

    move.w  #0x0007, 0xE82400
    .word   0xff00          | DOS _EXIT

| fill_gr: a0=開始アドレス %d0=幅-1 %d1=高さ-1 %d2.w=色コード、行送り2048バイト
fill_gr:
    move.w  %d1, %d5
.fg_row:
    move.w  %d0, %d4
    move.l  %a0, %a1
.fg_col:
    move.w  %d2, (%a1)+
    dbra    %d4, .fg_col
    add.l   #2048, %a0
    dbra    %d5, .fg_row
    rts
