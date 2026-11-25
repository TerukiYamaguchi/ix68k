| tools-x68k/fntadr/fntadr.s -- Issue #157: 現在有効なANKフォントの実アドレスを
| IOCS _FNTADR($16)で取得し、_B_PRINT($21)経由でエミュレータへ報告する。
|
| HIOCS PLUS等の常駐ドライバがANKフォントを実行時に差し替えていると、
| エミュレータのread_bitmap_text/run_commandはROM固定オフセットのCGROM
| パターンと照合するため文字を誤認識する(Issue #157)。本プログラムを実行すると
| 現在実際に使われているANKフォントパターンの先頭アドレスを
| "FNTADR=XXXXXXXX;"という行でコンソールへ出力する。エミュレータのiocs.c
| (_B_PRINTハンドラ)がこのマーカーを検出し、以後のread_bitmap_text/
| run_commandの文字認識をこのアドレス基準に切り替える。
|
| フォントを切り替えた直後(例: HIOCS C:\SYS\VAX.FON 実行後)に毎回実行すること。
| リセットすると報告は失われ、ROM固定オフセット参照に戻る。
|
| 呼び出し規約(ROM _FNTADR実装の逆アセンブル(dis/disasm/iplromxv)より、
| ANK/mode=8の場合):
|   IN:  D0.B=$16, D1.W=文字コード, D2.B=$08 (8x16 ANKパターン, 16バイト/文字。
|        x68k_bus_tvram_to_text()がTVRAM plane0から読む1文字ぶんと同じ形式)
|   OUT: D0.L=該当文字のパターン先頭アドレス
| D1=0を渡すと、返るD0がそのままテーブル先頭アドレス(= base + 0*16 = base)になる。

    .text
    .globl  _start
_start:
    moveq   #0, %d1             | 文字コード0
    move.b  #8, %d2             | mode=8 (8x16 ANK)
    move.w  #0x16, %d0          | IOCS _FNTADR
    trap    #15
    move.l  %d0, %d3            | d3 = ANKフォント先頭アドレス(以後d0は使い回す)

    | d3の32bitを8桁の大文字16進ASCIIへ変換し、hexplaceholderへ上位桁から書く
    lea     hexplaceholder, %a0
    moveq   #7, %d4             | 桁位置(7=最上位ニブル .. 0=最下位ニブル)
fntadr_hexloop:
    move.l  %d3, %d5
    move.w  %d4, %d6
    lsl.w   #2, %d6             | d6 = 桁位置*4 = シフト量
    lsr.l   %d6, %d5
    and.l   #0xF, %d5
    cmp.b   #9, %d5
    bhi     fntadr_hex_alpha
    add.b   #'0', %d5
    bra     fntadr_hex_store
fntadr_hex_alpha:
    add.b   #('A'-10), %d5
fntadr_hex_store:
    move.b  %d5, (%a0)+
    dbra    %d4, fntadr_hexloop

    | _B_PRINT(IOCS $21)でエミュレータへ報告(A1=0終端文字列アドレス)
    lea     msg, %a1
    move.w  #0x21, %d0
    trap    #15

    .word   0xff00               | DOS _EXIT

msg:
    .ascii  "FNTADR="
hexplaceholder:
    .ascii  "00000000"
    .ascii  ";\r\n"
    .byte   0
