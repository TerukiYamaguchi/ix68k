| tools-x68k/asmtest/sp_iocs_test.s — IOCS 0xC0-0xCF ROM委譲の動作確認（使い捨て）
| _SP_INIT/_SP_ON/_BGCTRLSTを呼び、結果とレジスタ実値をRAMに保存する。
| 結果保存先は 0x70000（搭載メモリ2MB=$200000未満の安全な低位アドレス。
| 旧版は0x200000ちょうど＝搭載メモリ境界外を指しておりバスエラーになっていた）。

    .text
    .globl _start
_start:
    move.w  #0, %d1          | mode0: 512x512 16色 1ページ 31kHz
    move.w  #0x10, %d0
    trap    #15              | _CRTMOD

    move.w  #0xC0, %d0
    trap    #15              | _SP_INIT
    move.w  %d0, 0x70000     | 結果(0=OK, -1=画面モード不正)

    move.w  #0xC1, %d0
    trap    #15              | _SP_ON
    move.w  %d0, 0x70002     | 結果

    move.l  #0, %d1          | BG0
    move.l  #0, %d2          | テキストページ0
    move.l  #1, %d3          | 表示ON
    move.w  #0xCA, %d0
    trap    #15              | _BGCTRLST
    move.w  %d0, 0x70004     | 結果

    move.w  0xeb0808, %d0    | BGコントロールレジスタの実値を確認
    move.w  %d0, 0x70006

    .word   0xff00           | DOS _EXIT
