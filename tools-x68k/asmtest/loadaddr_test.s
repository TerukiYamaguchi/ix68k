| loadaddr_test.s — 自分のロードアドレス(PC)とスタックのアドレスを表示する。
| 060loadhigh.x 経由で起動したときに $10000000 以上（ローカルメモリ）になることの確認用。
| 通常起動なら本体メモリ（$00C00000 未満）になる。

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
    lea     _start(%pc),%a0
    PRTV    m_pc,%a0
    PRTV    m_sp,%sp
    lea     work,%a1            | .bss 相当（データ領域）のアドレス
    PRTV    m_work,%a1
    cmpa.l  #0x10000000,%a0
    bcs     1f
    PRT     m_high
    bra     2f
1:  PRT     m_low
2:  .word   0xff00              | DOS _EXIT

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

m_pc:   .asciz  "code  (PC)   = $"
m_sp:   .asciz  "stack (SP)   = $"
m_work: .asciz  "data         = $"
m_high: .asciz  "HIGH: loaded in local memory\r\n"
m_low:  .asciz  "LOW: loaded in main memory\r\n"
    .even
    .data
work:   .long   0
