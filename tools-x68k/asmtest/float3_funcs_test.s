| tools-x68k/asmtest/float3_funcs_test.s — FLOAT3.X の FE コール（MC68881 経由）を直接呼ぶ。
|
| BASIC から届かない関数（SINH/COSH/TANH/ATANH/ASIN/ACOS/LOG10/LOG2）と FMOVECR π を、
| Line-F の FExx（FLOAT3.X の分岐表 L000220 の添字 = FExx）で呼び、結果の d0:d1 を手で置いた
| IEEE 754 倍精度の期待値と比べて「<名前> OK」か「<名前> NG」を DOS _PRINT で出す。
| 期待値は exact に表せる結果（0・1・2・3）と π/2・π のみ。
| 使い方: tests/test_fpu_cir_float3_e2e.py が FLOAT3.XDF の複製へ build.sh で書き込んで実行する。

    .macro CHECK code, hi, lo, ehi, elo, name
    move.l  #\hi, %d0
    move.l  #\lo, %d1
    .word   \code
    cmp.l   #\ehi, %d0
    bne     1f
    cmp.l   #\elo, %d1
    bne     1f
    pea     2f(%pc)
    bra     3f
1:  pea     4f(%pc)
3:  .word   0xff09               | DOS _PRINT
    addq.l  #4, %sp
    bra     5f
2:  .asciz  "\name OK\r\n"
4:  .asciz  "\name NG\r\n"
    .even
5:
    .endm

    .text
    .globl _start
_start:
    CHECK 0xfe41, 0x00000000, 0, 0x00000000, 0, FSINH       | sinh 0 = 0
    CHECK 0xfe42, 0x00000000, 0, 0x3ff00000, 0, FCOSH       | cosh 0 = 1
    CHECK 0xfe43, 0x00000000, 0, 0x00000000, 0, FTANH       | tanh 0 = 0
    CHECK 0xfe44, 0x00000000, 0, 0x00000000, 0, FATANH      | atanh 0 = 0
    CHECK 0xfe45, 0x00000000, 0, 0x00000000, 0, FASIN       | asin 0 = 0
    CHECK 0xfe46, 0x00000000, 0, 0x3ff921fb, 0x54442d18, FACOS | acos 0 = pi/2
    CHECK 0xfe47, 0x40590000, 0, 0x40000000, 0, FLOG10      | log10 100 = 2
    CHECK 0xfe48, 0x40200000, 0, 0x40080000, 0, FLOG2       | log2 8 = 3
    CHECK 0xfe3e, 0x3ff00000, 0, 0x400921fb, 0x54442d18, FPIMUL | FMOVECR pi * 1
    move.l  #0x00000000, %d0
    .word   0xff00                                          | DOS _EXIT
