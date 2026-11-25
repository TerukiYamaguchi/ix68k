| tools-x68k/asmtest/t060unimp.s -- MC68060 未実装整数命令(ベクタ61 -> 060SP)の動作確認
|
| 060turbo.sys導入環境で MOVEP / CAS2 / 64bit MULU.L / 64bit DIVU.L を実行し、
| 060SPのエミュレーション結果が68020以降の仕様どおりかを確認する。
| 各ケースの結果を DOS _PRINT で "n:OK" / "n:NG D0 D1 D2 buf[0] buf[4]=<実測値>" と出力する。

    .text
    .globl _start
_start:
    | 1. MOVEP.L D0,0(A0): 偶数バイトだけが書かれる
    lea     buf(%pc), %a0
    clr.l   (%a0)
    clr.l   4(%a0)
    clr.l   8(%a0)
    move.l  #0x12345678, %d0
    movep.l %d0, 0(%a0)
    lea     m1(%pc), %a1
    cmp.l   #0x12003400, (%a0)
    bne.s   fail1
    cmp.l   #0x56007800, 4(%a0)
    bne.s   fail1
    bsr     pass
    bra.s   t2
fail1:
    bsr     fail

t2:
    | 2. MOVEP.L 0(A0),D1: 読み戻し
    lea     buf(%pc), %a0
    moveq   #0, %d1
    movep.l 0(%a0), %d1
    lea     m2(%pc), %a1
    cmp.l   #0x12345678, %d1
    bne.s   fail2
    bsr     pass
    bra.s   t3
fail2:
    bsr     fail

t3:
    | 3. CAS2.L: 比較一致時に更新される
    lea     buf(%pc), %a0
    lea     4(%a0), %a1
    move.l  #1, (%a0)
    move.l  #2, (%a1)
    moveq   #1, %d1
    moveq   #2, %d2
    move.l  #0xAA, %d3
    move.l  #0xBB, %d4
    cas2.l  %d1:%d2, %d3:%d4, (%a0):(%a1)
    lea     m3(%pc), %a1
    cmp.l   #0xAA, (%a0)
    bne.s   fail3
    lea     buf+4(%pc), %a0
    cmp.l   #0xBB, (%a0)
    bne.s   fail3
    bsr     pass
    bra.s   t4
fail3:
    bsr     fail

t4:
    | 4. MULU.L 64bit: 0xFFFFFFFF * 0x10 = 0xF_FFFFFFF0
    move.l  #0xFFFFFFFF, %d0
    moveq   #0x10, %d1
    mulu.l  %d1, %d2:%d0
    lea     m4(%pc), %a1
    cmp.l   #0xF, %d2
    bne.s   fail4
    cmp.l   #0xFFFFFFF0, %d0
    bne.s   fail4
    bsr     pass
    bra.s   t5
fail4:
    bsr     fail

t5:
    | 5. DIVU.L 64bit: 0x1_00000000 / 0x10 = 0x10000000 余り0
    moveq   #1, %d2
    moveq   #0, %d0
    moveq   #0x10, %d1
    divu.l  %d1, %d2:%d0
    lea     m5(%pc), %a1
    cmp.l   #0x10000000, %d0
    bne.s   fail5
    tst.l   %d2
    bne.s   fail5
    bsr     pass
    bra.s   done
fail5:
    bsr     fail

done:
    .word   0xff00            | DOS _EXIT

| A1 = メッセージ("n:")。pass/fail は続けて OK/NG を出力する
pass:
    move.l  %a1, -(%sp)
    .word   0xff09            | DOS _PRINT
    addq.l  #4, %sp
    pea     okm(%pc)
    .word   0xff09
    addq.l  #4, %sp
    rts
fail:
    movem.l %d0-%d2, -(%sp)     | 失敗時点の D0-D2 を実測値として表示する
    move.l  %a1, -(%sp)
    .word   0xff09
    addq.l  #4, %sp
    pea     ngm(%pc)
    .word   0xff09
    addq.l  #4, %sp
    move.l  (%sp), %d0
    bsr     hex
    move.l  4(%sp), %d0
    bsr     hex
    move.l  8(%sp), %d0
    bsr     hex
    lea     buf(%pc), %a0           | メモリ側の実測値 buf[0], buf[4]
    move.l  (%a0), %d0
    bsr     hex
    lea     buf(%pc), %a0
    move.l  4(%a0), %d0
    bsr     hex
    pea     crlf(%pc)
    .word   0xff09
    addq.l  #4, %sp
    lea     12(%sp), %sp
    rts

| D0 を8桁16進 + 空白で表示
hex:
    lea     hbuf(%pc), %a0
    moveq   #7, %d2
1:  rol.l   #4, %d0
    move.b  %d0, %d1
    and.b   #15, %d1
    cmp.b   #10, %d1
    bcs.s   2f
    add.b   #7, %d1
2:  add.b   #'0', %d1
    move.b  %d1, (%a0)+
    dbra    %d2, 1b
    move.b  #32, (%a0)+
    clr.b   (%a0)
    pea     hbuf(%pc)
    .word   0xff09
    addq.l  #4, %sp
    rts

    .data
m1: .asciz "1:"
m2: .asciz "2:"
m3: .asciz "3:"
m4: .asciz "4:"
m5: .asciz "5:"
okm: .asciz "OK\r\n"
ngm: .asciz "NG D0 D1 D2 buf0 buf4="
crlf: .asciz "\r\n"
    .even
buf: .space 16
hbuf: .space 16
