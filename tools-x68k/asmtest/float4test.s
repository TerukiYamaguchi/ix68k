| FLOAT4.X の FE ディスパッチを FPCP 経由で回帰する自動起動用 Human68k .X。
| IOCS _B_LPEEK で vector $20 の直前にある FEfn を確認した時だけ FE を発行する。
| 結果は a0、サイズは d0、MAGIC は d1 に載せて ILLEGAL で停止する。

    .equ MAGIC, 0x46345453             | "F4TS"
    .equ IOCS_B_LPEEK, 0x84
    .equ FEFN, 0x4645666e

    .text
    .globl _start
_start:
    lea result(%pc), %a4
    move.l #MAGIC, (%a4)

    | vector $20 を安全に読み、ハンドラ直前の FEfn 署名を調べる。
    movea.l #0x20, %a1
    move.w #IOCS_B_LPEEK, %d0
    trap #15
    move.l %d0, 8(%a4)
    movea.l %d0, %a1
    subq.l #4, %a1
    move.w #IOCS_B_LPEEK, %d0
    trap #15
    cmp.l #FEFN, %d0
    bne not_resident
    move.l #1, 4(%a4)

    | IPLROM30 が設定した FPU フラグと F-line ベクタも記録する。
    movea.l #0xcbd, %a1
    move.w #IOCS_B_LPEEK, %d0
    trap #15
    move.l %d0, 16(%a4)
    movea.l #0xcbe, %a1
    move.w #IOCS_B_LPEEK, %d0
    trap #15
    move.l %d0, 20(%a4)
    movea.l #0x2c, %a1
    move.w #IOCS_B_LPEEK, %d0
    trap #15
    move.l %d0, 12(%a4)

    | DADD(1.5, 2.5) = 4.0。d0:d1 / d2:d3 が FLOAT4 ABI の倍精度引数。
    move.l #0x3ff80000, %d0
    clr.l %d1
    move.l #0x40040000, %d2
    clr.l %d3
    .word 0xfe2b
    move.l %d0, 24(%a4)
    move.l %d1, 28(%a4)

    | DMUL(1.5, 2.5) = 3.75。
    move.l #0x3ff80000, %d0
    clr.l %d1
    move.l #0x40040000, %d2
    clr.l %d3
    .word 0xfe2d
    move.l %d0, 32(%a4)
    move.l %d1, 36(%a4)

    | DCMP(4.0, 4.0)、DFIX(3.75)、SIN(1.0) も同じ FE 経路で確認する。
    move.l #0x40100000, %d0
    clr.l %d1
    move.l #0x40100000, %d2
    clr.l %d3
    .word 0xfe29
    move.l %d0, 40(%a4)
    move.l #0x400e0000, %d0
    clr.l %d1
    .word 0xfe32
    move.l %d0, 44(%a4)
    move.l #0x3ff00000, %d0
    clr.l %d1
    .word 0xfe36
    move.l %d0, 48(%a4)
    move.l %d1, 52(%a4)

not_resident:
    movea.l %a4, %a0
    moveq #56, %d0
    move.l #MAGIC, %d1
    illegal

    .even
result:
    .space 56
