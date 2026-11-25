| tools-x68k/asmtest/dmamove_test.s -- IOCS $8A-$8D (_DMAMOVE/_DMAMOV_A/_DMAMOV_L/_DMAMODE)
| ROM委譲 + DMAC ch2実装の実機検証
|
| メインRAMは0x000000-0x0FFFFFの1MBのみ。出力は末尾寄りの0xF8000以降に固定
| 配置する（プログラム自体・他テストと衝突しない領域、fntget_test.s踏襲）。
|
| 0xF8000: Test1 (_DMAMOVE 単純転送) コピー結果 10バイト ("HELLO,DMA!")
| 0xF8010: Test1直後の _DMAMODE 結果 (long, 0なら即完了)
| 0xF8020: Test2 (2回目の_DMAMOVE呼び出し) コピー結果 6バイト ("SECOND")
|          -- ここが正しく書き変わっていれば、1回目のDMAC ch2完了割り込みが
|             正しく届いて$C34ビジーウェイトが解けたことの証明
| 0xF8030: Test2直後の _DMAMODE 結果
| 0xF8040: Test3 (_DMAMOV_A アレイチェーン、2エントリ) コピー結果 8バイト
|          ("ABCDWXYZ" になれば成功)
| 0xF8050: Test3直後の _DMAMODE 結果

    .text
    .globl _start
_start:
    | --- Test1: _DMAMOVE 単純前方転送 ---
    lea     src1, %a1
    lea     0xF8000, %a2
    move.l  #10, %d2
    move.l  #0x00000005, %d1   | dir=0(a1->a2) mac=01(a1++) dac=01(a2++)
    move.w  #0x8a, %d0
    trap    #15
    move.w  #0x8d, %d0
    trap    #15
    move.l  %d0, 0xF8010

    | --- Test2: 2回目の_DMAMOVE呼び出し（IRQ配線の検証） ---
    lea     src2, %a1
    lea     0xF8020, %a2
    move.l  #6, %d2
    move.l  #0x00000005, %d1
    move.w  #0x8a, %d0
    trap    #15
    move.w  #0x8d, %d0
    trap    #15
    move.l  %d0, 0xF8030

    | --- Test3: _DMAMOV_A アレイチェーン(2エントリ) ---
    lea     chain_table, %a1
    lea     0xF8040, %a2
    move.l  #2, %d2            | チェーンエントリ数(BTC)
    move.l  #0x00000005, %d1   | dir=0 mac=01(各ブロック内で自動++) dac=01(a2++で連続配置)
    move.w  #0x8b, %d0
    trap    #15
    move.w  #0x8d, %d0
    trap    #15
    move.l  %d0, 0xF8050

    .word   0xff00              | DOS _EXIT

src1:
    .ascii  "HELLO,DMA!"
src2:
    .ascii  "SECOND"
chain_table:
    .long   chain_src_a
    .word   4
    .long   chain_src_b
    .word   4
chain_src_a:
    .ascii  "ABCD"
chain_src_b:
    .ascii  "WXYZ"
