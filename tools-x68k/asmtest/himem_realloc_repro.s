| himem_realloc_repro.s — 060turbo.sys の IOCS _HIMEM REALLOC(7) が、ブロックを
| 移動せずにサイズ変更できたとき a1 に「新しい末尾」を返す不具合の最小再現。
|
| 期待: a1 = 旧ブロックの先頭（移動なしなので旧ポインタと同じ）
| 実際: a1 = 旧ブロックの先頭 + 新サイズ
|
| 手順: VERSION 確認 -> 64KB を確保 -> 32KB に縮小 REALLOC -> 旧ポインタと返り値を表示
|       -> 返り値で FREE（失敗する）-> 旧ポインタで FREE（成功する）。
| 縮小は移動なしで成功するはずなので、a1 が旧ポインタと違えば不具合。
| MALLOC/REALLOC の d0 を確認し、失敗時は不具合と区別できるよう別メッセージで終了する。
|
| ビルド: m68k-xelf-as -m68000 -o r.o himem_realloc_repro.s &&
|         m68k-xelf-gcc -m68000 -nostdlib -nostartfiles -Wl,-e,_start -o r.elf r.o &&
|         elf2x68k.py -o REPRO.X r.elf

    .text
    .globl _start

    .macro  PRT m
    pea     \m(%pc)
    .word   0xff09              | DOS _PRINT
    addq.l  #4,%sp
    .endm

    .macro  PRTV m, r           | メッセージ + 値(8桁16進)
    move.l  \r,-(%sp)
    PRT     \m
    move.l  (%sp)+,%d0
    bsr     phex
    .endm

    .macro  HIMEM cmd
    moveq   #\cmd,%d1
    move.l  #0xF8,%d0           | IOCS _HIMEM
    trap    #15
    .endm

_start:
    HIMEM   5                   | HIMEM_VERSION
    cmp.l   #0x30363054,%d0     | '060T'
    bne     fail_version

    move.l  #0x10000,%d2
    HIMEM   1                   | HIMEM_MALLOC 64KB
    tst.l   %d0
    bne     fail_malloc
    move.l  %a1,%d7             | 旧ポインタ

    move.l  %d7,%d2
    move.l  #0x8000,%d3         | 新サイズ 32KB
    HIMEM   7                   | HIMEM_REALLOC
    move.l  %a1,%d6             | 返り値
    tst.l   %d0
    bne     fail_realloc

    PRTV    m_old,%d7
    PRTV    m_new,%d6
    cmp.l   %d7,%d6
    beq     1f
    PRT     m_ng
    bra     2f
1:  PRT     m_ok

2:  move.l  %d6,%d2             | 返り値で FREE
    HIMEM   2
    PRTV    m_free_new,%d0
    move.l  %d7,%d2             | 旧ポインタで FREE
    HIMEM   2
    PRTV    m_free_old,%d0
    .word   0xff00              | DOS _EXIT

fail_version:
    PRT     m_fail_version
    .word   0xff00
fail_malloc:
    PRT     m_fail_malloc
    .word   0xff00
fail_realloc:
    PRT     m_fail_realloc
    .word   0xff00

phex:
    movem.l %d0-%d2/%a0,-(%sp)
    lea     -12(%sp),%sp
    lea     (%sp),%a0
    moveq   #7,%d2
1:  rol.l   #4,%d0
    move.l  %d0,%d1
    and.l   #15,%d1
    add.b   #'0',%d1
    cmp.b   #'9',%d1
    ble     2f
    add.b   #7,%d1
2:  move.b  %d1,(%a0)+
    dbra    %d2,1b
    move.b  #13,(%a0)+
    move.b  #10,(%a0)+
    clr.b   (%a0)
    pea     (%sp)
    .word   0xff09
    addq.l  #4,%sp
    lea     12(%sp),%sp
    movem.l (%sp)+,%d0-%d2/%a0
    rts

m_old:          .asciz  "old ptr            = $"
m_new:          .asciz  "REALLOC a1         = $"
m_ok:           .asciz  "OK: a1 == old ptr\r\n"
m_ng:           .asciz  "NG: a1 != old ptr\r\n"
m_free_new:     .asciz  "FREE(REALLOC a1) d0 = $"
m_free_old:     .asciz  "FREE(old ptr)     d0 = $"
m_fail_version: .asciz  "ERROR: HIMEM_VERSION is not '060T'\r\n"
m_fail_malloc:  .asciz  "ERROR: HIMEM_MALLOC failed\r\n"
m_fail_realloc: .asciz  "ERROR: HIMEM_REALLOC failed\r\n"
    .even
