| tools-x68k/asmtest/r1_default_check.s -- CRTMOD+G_CLR_ON直後のR1(ビデオコントローラ$E82500)
| 自然値をGETCHARで一時停止して確認する（TX/GR優先順位のデフォルトを知るため）

    .text
    .globl  _start
_start:
    move.w  #0x10, %d0
    move.w  #0, %d1
    trap    #15
    move.w  #0x90, %d0
    trap    #15
    .word   0xff01          | GETCHAR (一時停止、R1をMCPで読む)
    .word   0xff00          | DOS _EXIT
