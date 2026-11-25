| tools-x68k/asmtest/tick.s -- IOCS _ONTIME($7F)の値を「秒.1/100秒」で表示する計時用
|
| 速度比較（BENCH 等）の前後で実行し、表示値の差を所要時間とする。
|   TICK
|   BENCH
|   TICK
| _ONTIME はゲスト単調時計（ホスト壁時計基準、デバッガ停止中は凍結）の経過を返す。
| 分解能は 10ms。D0=当日分の経過 1/100 秒、D1=経過日数（本プログラムは D1 を表示しない）。

    .macro  PRT m
    pea     \m(%pc)
    .word   0xff09              | DOS _PRINT
    addq.l  #4,%sp
    .endm

    .text
    .globl  _start
_start:
    move.w  #0x7F,%d0           | IOCS _ONTIME
    trap    #15                 | D0=1/100秒単位の経過
    lea     numend(%pc),%a0     | 数字は末尾から逆順に詰める
    moveq   #0,%d3              | 出力済み桁数（'.' は数えない）
digit_loop:
    | D0(32bit) を 10 で割る。32bit÷16bit の商が 16bit に収まらないため上下 16bit に分けて割る
    move.l  %d0,%d2
    swap    %d2
    andi.l  #0xFFFF,%d2         | 上位ワード
    divu.w  #10,%d2             | D2 = (上位%10)<<16 | 上位/10
    move.w  %d2,%d4
    andi.l  #0xFFFF,%d4         | D4 = 新しい上位ワード（商）
    move.w  %d0,%d2             | D2 = (上位%10)<<16 | 下位ワード
    divu.w  #10,%d2             | D2 = 余り<<16 | 商の下位
    move.l  %d2,%d5
    swap    %d5                 | D5.w = 余り（この桁）
    andi.l  #0xFFFF,%d2         | D2 = 商の下位
    swap    %d4
    move.l  %d4,%d0
    or.l    %d2,%d0             | D0 = 商
    addi.b  #'0',%d5
    move.b  %d5,-(%a0)
    addq.w  #1,%d3
    cmpi.w  #2,%d3
    bne     1f
    move.b  #'.',-(%a0)         | 下位 2 桁の前に小数点
1:  tst.l   %d0
    bne     digit_loop
    cmpi.w  #3,%d3              | 0.05 のように整数部が 0 でも "0" を出す
    blt     digit_loop
    PRT     msg_head
    move.l  %a0,-(%sp)
    .word   0xff09              | DOS _PRINT（数字列）
    addq.l  #4,%sp
    PRT     msg_tail
    .word   0xff00              | DOS _EXIT

msg_head:
    .asciz  "ONTIME "
msg_tail:
    .asciz  " sec\r\n"
numbuf:
    .space  15
numend:
    .byte   0
    .even
