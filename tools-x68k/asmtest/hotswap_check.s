| tools-x68k/asmtest/hotswap_check.s -- ホットスワップ後に新規実行ファイルが動くか確認用
    .text
    .globl  _start
_start:
    lea     MSG, %a1
    move.w  #0x21, %d0    | _B_PRINT
    trap    #15
    .word   0xff00         | DOS _EXIT
MSG:
    .ascii  "HOTSWAP_OK\0"
    .even
