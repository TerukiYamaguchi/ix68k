| tools-x68k/asmtest/crtmod_test.s -- _CRTMOD 代表11モードの描画/スクロール/優先順位切替 動作検証
|
| GETCHAR ($ff01, エコーあり) でチェックポイント毎に一時停止する。
| MCP の key(action="press", ...) で1文字送って進める。各PAUSE直前が
| screenshotの撮影ポイント。
|
| モードごとの流れ:
|   1. _CRTMOD(mode) + _G_CLR_ON            (二段構え初期化が必要)
|   2. R1(優先順位)=$E4(恒等)・スクロールR12-19=0にリセット、パレット idx1-4=赤/緑/青/白
|   3. 各ページのコーナーに色分け64x64四角を描画 → PAUSE (描画チェック)
|   4. page0のスクロールXを+100 → PAUSE (スクロールチェック)
|   5. (複数ページのみ) page0スクロールを戻し、全ページの中央同一座標に
|      色分け四角を描画 → PAUSE (優先順位デフォルト = page0前面のはず)
|   6. (複数ページのみ) R1書き換えでpage1を最前面に → PAUSE (優先順位切替後)
|
| 対象モード: 0,2,16,17,18 (実画面1024, 16色1ページ)
|             12,14        (実画面512, 65536色1ページ、直接色)
|             4,6          (実画面512, 16色4ページ)
|             8,10         (実画面512, 256色2ページ)

    .macro CRTMOD_INIT mode
    move.w  #0x10, %d0
    move.w  #\mode, %d1
    trap    #15
    move.w  #0x90, %d0
    trap    #15
    move.w  #0xE4, 0xE82500
    move.w  #0, 0xE80018
    move.w  #0, 0xE8001A
    move.w  #0, 0xE8001C
    move.w  #0, 0xE8001E
    move.w  #0, 0xE80020
    move.w  #0, 0xE80022
    move.w  #0, 0xE80024
    move.w  #0, 0xE80026
    move.w  #0x07C0, 0xE82002
    move.w  #0xF800, 0xE82004
    move.w  #0x003E, 0xE82006
    move.w  #0xFFFE, 0xE82008
    .endm

    .macro FILL addr,w,h,color,rowdelta
    lea     \addr, %a0
    move.w  #\w-1, %d0
    move.w  #\h-1, %d1
    move.w  #\color, %d2
    move.l  #\rowdelta, %d3
    jsr     fill_sq
    .endm

    .macro PAUSE
    .word   0xff01
    .endm

    .text
    .globl  _start
_start:
    | カーソルを画面下部(col0,row23)へ逃がす(GETCHARエコー文字が描画確認域に被らないように)
    move.w  #0x23, %d0
    move.w  #0, %d1
    move.w  #23, %d2
    trap    #15

    | ===== mode 0: 実画面1024/表示512x512/16色1ページ/31kHz =====
    CRTMOD_INIT 0
    FILL    0xC04010,32,32,1,1984
    FILL    0xC041B0,32,32,2,1984
    FILL    0xC6C010,32,32,3,1984
    FILL    0xC6C1B0,32,32,4,1984
    PAUSE
    move.w  #100, 0xE80018
    PAUSE

    | ===== mode 2: 実画面1024/表示256x256/16色1ページ/31kHz =====
    CRTMOD_INIT 2
    FILL    0xC04010,32,32,1,1984
    FILL    0xC041B0,32,32,2,1984
    FILL    0xC6C010,32,32,3,1984
    FILL    0xC6C1B0,32,32,4,1984
    PAUSE
    move.w  #100, 0xE80018
    PAUSE

    | ===== mode 16: 実画面1024/表示768x512/16色1ページ/31kHz =====
    CRTMOD_INIT 16
    FILL    0xC04010,32,32,1,1984
    FILL    0xC041B0,32,32,2,1984
    FILL    0xC6C010,32,32,3,1984
    FILL    0xC6C1B0,32,32,4,1984
    PAUSE
    move.w  #100, 0xE80018
    PAUSE

    | ===== mode 17: 実画面1024/表示1024x424/16色1ページ/24kHz =====
    CRTMOD_INIT 17
    FILL    0xC04010,32,32,1,1984
    FILL    0xC041B0,32,32,2,1984
    FILL    0xC6C010,32,32,3,1984
    FILL    0xC6C1B0,32,32,4,1984
    PAUSE
    move.w  #100, 0xE80018
    PAUSE

    | ===== mode 18: 実画面1024/表示1024x848/16色1ページ/24kHz =====
    CRTMOD_INIT 18
    FILL    0xC04010,32,32,1,1984
    FILL    0xC041B0,32,32,2,1984
    FILL    0xC6C010,32,32,3,1984
    FILL    0xC6C1B0,32,32,4,1984
    PAUSE
    move.w  #100, 0xE80018
    PAUSE

    | ===== mode 12: 実画面512/表示512x512/65536色1ページ/31kHz (直接色) =====
    CRTMOD_INIT 12
    FILL    0xC02010,32,32,0x07C0,960
    FILL    0xC021B0,32,32,0xF800,960
    FILL    0xC36010,32,32,0x003E,960
    FILL    0xC361B0,32,32,0xFFFE,960
    PAUSE
    move.w  #100, 0xE80018
    PAUSE

    | ===== mode 14: 実画面512/表示256x256/65536色1ページ/31kHz (直接色) =====
    CRTMOD_INIT 14
    FILL    0xC02010,32,32,0x07C0,960
    FILL    0xC021B0,32,32,0xF800,960
    FILL    0xC36010,32,32,0x003E,960
    FILL    0xC361B0,32,32,0xFFFE,960
    PAUSE
    move.w  #100, 0xE80018
    PAUSE

    | ===== mode 4: 実画面512/表示512x512/16色4ページ/31kHz =====
    CRTMOD_INIT 4
    FILL    0xC02010,32,32,1,960
    FILL    0xC821B0,32,32,2,960
    FILL    0xD36010,32,32,3,960
    FILL    0xDB61B0,32,32,4,960
    PAUSE
    move.w  #100, 0xE80018
    PAUSE
    move.w  #0, 0xE80018
    FILL    0xC1C0E0,32,32,1,960
    FILL    0xC9C0E0,32,32,2,960
    FILL    0xD1C0E0,32,32,3,960
    FILL    0xD9C0E0,32,32,4,960
    PAUSE
    move.w  #0xE1, 0xE82500
    PAUSE

    | ===== mode 6: 実画面512/表示256x256/16色4ページ/31kHz =====
    CRTMOD_INIT 6
    FILL    0xC02010,32,32,1,960
    FILL    0xC821B0,32,32,2,960
    FILL    0xD36010,32,32,3,960
    FILL    0xDB61B0,32,32,4,960
    PAUSE
    move.w  #100, 0xE80018
    PAUSE
    move.w  #0, 0xE80018
    FILL    0xC1C0E0,32,32,1,960
    FILL    0xC9C0E0,32,32,2,960
    FILL    0xD1C0E0,32,32,3,960
    FILL    0xD9C0E0,32,32,4,960
    PAUSE
    move.w  #0xE1, 0xE82500
    PAUSE

    | ===== mode 8: 実画面512/表示512x512/256色2ページ/31kHz =====
    CRTMOD_INIT 8
    FILL    0xC02010,32,32,1,960
    FILL    0xC821B0,32,32,2,960
    PAUSE
    move.w  #100, 0xE80018
    PAUSE
    move.w  #0, 0xE80018
    FILL    0xC1C0E0,32,32,1,960
    FILL    0xC9C0E0,32,32,2,960
    PAUSE
    move.w  #0x4E, 0xE82500
    PAUSE

    | ===== mode 10: 実画面512/表示256x256/256色2ページ/31kHz =====
    CRTMOD_INIT 10
    FILL    0xC02010,32,32,1,960
    FILL    0xC821B0,32,32,2,960
    PAUSE
    move.w  #100, 0xE80018
    PAUSE
    move.w  #0, 0xE80018
    FILL    0xC1C0E0,32,32,1,960
    FILL    0xC9C0E0,32,32,2,960
    PAUSE
    move.w  #0x4E, 0xE82500
    PAUSE

    .word   0xff00          | DOS _EXIT

| fill_sq: a0=開始アドレス(左上ワード) d0=幅-1 d1=高さ-1 d2.w=書き込む値
|          d3.l=行送りバイト数(1行分の総バイト数 - 幅*2)
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
