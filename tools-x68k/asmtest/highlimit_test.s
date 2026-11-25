| highlimit_test.s — 自分のロードアドレス(PC)と、DOS _MALLOC で確保した 64KB の
| アドレスを表示する。060high.x の領域制限（ロード/アロケート）の確認用。
| $10000000 以上ならローカルメモリ（ハイメモリ）、未満なら本体メモリ。

    .text
    .globl _start

    .macro  PRT m
    pea     \m(%pc)
    .word   0xff09              | DOS _PRINT
    addq.l  #4,%sp
    .endm

    .macro  PRTV m, r
    move.l  \r,-(%sp)
    PRT     \m
    move.l  (%sp)+,%d0
    bsr     phex
    .endm

_start:
    lea     16(%a0),%a1         | a0 = 自プロセスのメモリ管理ポインタ。+16 がブロック先頭
    move.l  #0x20000,-(%sp)     | 自ブロックを 128KB に縮める（既定では空きを全部持っている）
    move.l  %a1,-(%sp)
    .word   0xff4a              | DOS _SETBLOCK
    addq.l  #8,%sp
    lea     _start(%pc),%a0
    PRTV    m_pc,%a0
    move.l  #0x10000,-(%sp)     | 64KB
    .word   0xff48              | DOS _MALLOC
    addq.l  #4,%sp
    move.l  %d0,%d3
    PRTV    m_alloc,%d3
    tst.l   %d3
    bmi     1f
    move.l  %d3,-(%sp)
    .word   0xff49              | DOS _MFREE
    addq.l  #4,%sp
1:  .word   0xff00              | DOS _EXIT

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

m_pc:    .asciz  "code  (PC)   = $"
m_alloc: .asciz  "alloc (64KB) = $"
    .even
