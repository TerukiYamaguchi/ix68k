| tools-x68k/asmtest/drvchk8_probe.s -- _B_DRVCHK($4E) mode=8 (排出検査) の戻り値を
| 固定RAMアドレスに書いて即終了する（GETCHAR待ちにしない。read_memoryで確認）。
|
| d1.hb = PDA (1 = ドライブB:), d2.w = 8 (排出検査)
| 返値: d0.w = 1 (前回検査後イジェクトしていない) / -1 (イジェクトした)

    .text
    .globl  _start
_start:
    move.w  #0x4E, %d0
    move.w  #0x0100, %d1   | PDA=1 (B:)
    move.w  #8, %d2         | mode 8: 排出検査
    trap    #15
    move.w  %d0, 0x0FF000   | 結果を固定アドレスに保存（RAM_SIZE=1MBの範囲内）
    .word   0xff00           | DOS _EXIT
