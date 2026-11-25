| tools-x68k/asmtest/scan_equal_test.s -- IOCS _B_VERIFY(0x41)/FDC SCAN EQUAL
| 実機検証(Issue #165設計書 §5.3)。FD1(B:, PDA=$91)へ1024バイト書き込んでから
| 複数パターンでベリファイし、結果を固定RAMアドレスへ並べて保存する。
|
| D1.w = PDA<<8 | flags($70=MFM+retry+seek, $30=FM+retry+seek)
| D2.l = N<<24 | C<<16 | H<<8 | R
| D3.l = バイト数, A1 = バッファ
|
| 結果レイアウト($0FF000起点、各4バイト):
|   +0x00: (a) _B_WRITE の戻り値
|   +0x04: (a) 一致ベリファイ(同一データ) -> 期待 $00000801 (ST2=SH,C=1)
|   +0x08: (b) 不一致ベリファイ(1バイト反転) -> 期待 $00000401 (ST2=SN)
|   +0x0C: (c) 目的データ全$FF(ワイルドカード) -> 期待 $00000801 (SH)
|   +0x10: (g) R=0 -> 期待 $FFFFFFFF
|   +0x14: (h) R=9 -> 期待 $FFFFFFFF
|   +0x18: (i) N=4 -> 期待 $FFFFFFFF
|   +0x1C: (j) N=3+FM($9030) -> 期待 $FFFFFFFF
|   +0x20: (k) C=80(シリンダ範囲外) -> 期待 $4004xxxx(ST0=AT,ST1=ND)

    .text
    .globl  _start
_start:
    | ---- 1024バイトのパターンをバッファへ用意 ($0F0000) ----
    lea     0x0F0000, %a2
    move.w  #1023, %d5
fill_pattern:
    move.w  %d5, %d4
    move.b  %d4, (%a2)+
    dbra    %d5, fill_pattern

    | ---- (a) FD1 C0H0R1へ1024バイト書き込む ----
    move.w  #0x46, %d0        | _B_WRITE
    move.w  #0x9170, %d1
    move.l  #0x03000001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF000

    | ---- (a) 一致ベリファイ ----
    move.w  #0x41, %d0        | _B_VERIFY
    move.w  #0x9170, %d1
    move.l  #0x03000001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF004

    | ---- (b) 1バイト反転して不一致ベリファイ ----
    lea     0x0F0000, %a2
    not.b   (%a2)
    move.w  #0x41, %d0
    move.w  #0x9170, %d1
    move.l  #0x03000001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF008
    lea     0x0F0000, %a2
    not.b   (%a2)              | 元に戻す

    | ---- (c) 目的データ全$FF(ワイルドカード) ----
    lea     0x0F0000, %a2
    move.w  #1023, %d5
fill_ff:
    move.b  #0xFF, (%a2)+
    dbra    %d5, fill_ff
    move.w  #0x41, %d0
    move.w  #0x9170, %d1
    move.l  #0x03000001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF00C

    | ---- (g) R=0 ----
    move.w  #0x41, %d0
    move.w  #0x9170, %d1
    move.l  #0x03000000, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF010

    | ---- (h) R=9 ----
    move.w  #0x41, %d0
    move.w  #0x9170, %d1
    move.l  #0x03000009, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF014

    | ---- (i) N=4 ----
    move.w  #0x41, %d0
    move.w  #0x9170, %d1
    move.l  #0x04000001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF018

    | ---- (j) N=3 + FM指定($9030) ----
    move.w  #0x41, %d0
    move.w  #0x9030, %d1
    move.l  #0x03000001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF01C

    | ---- (k) シリンダ範囲外 C=80(0x50) ----
    move.w  #0x41, %d0
    move.w  #0x9170, %d1
    move.l  #0x03500001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, 0x0FF020

    .word   0xff00              | DOS _EXIT
