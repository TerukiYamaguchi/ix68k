| tools-x68k/asmtest/t060fpu.s -- MC68060 FPU 未実装命令(ベクタ11/60 -> FPSP)の動作確認
|
| 060turbo.sys導入環境(MC68060 + FPU)で、060が直接実行しない FP 命令が 060SP の FPSP で
| 正しく実行されるかを確認する。各ケースの結果を "n:OK" / "n:NG" で出力する。
|   1: FCOS.X   (Table 6-11 の未実装命令 -> ベクタ11)   cos(0)  = 1
|   2: FETOX.X  (同上)                                   e^0     = 1
|   3: FMOVEM.X Dn,-(A7) / (A7)+,Dn (動的リスト -> ベクタ60, format $0)
|   4: FADD.X #imm,FP0 (拡張精度即値 -> ベクタ60)        0 + 2.5 = 2.5 -> 整数化して 2(最近接偶数)
| 1,2 は拡張精度(96bit)のまま 1.0 (3FFF0000 80000000 00000000) と完全一致を検査する。
| NG のときは3ロングを16進表示する(1行目=先頭、続く2行=仮数)。

    .text
    .globl _start
_start:
    | 1. FCOS.X
    fmove.l #0, %fp0
    fcos.x  %fp0, %fp0
    lea     m1(%pc), %a1
    bsr     isone
    bne.s   f1
    bsr     pass
    bra.s   t2
f1: bsr     fail
    bsr     dumpx

t2: | 2. FETOX.X
    fmove.l #0, %fp0
    fetox.x %fp0, %fp0
    lea     m2(%pc), %a1
    bsr     isone
    bne.s   f2
    bsr     pass
    bra.s   t3
f2: bsr     fail
    bsr     dumpx

t3: | 3. FMOVEM.X 動的リスト(書き出し -> 別レジスタへ読み戻し)
    fmove.l #7, %fp0
    moveq   #1, %d1             | predecrement のリストは bit0 = FP0
    fmovem.x %d1, -(%sp)
    fmove.l #0, %fp0
    moveq   #-128, %d2          | postincrement のリストは bit7 = FP0
    fmovem.x (%sp)+, %d2
    fmove.l %fp0, %d0
    move.l  %d0, %d6
    lea     m3(%pc), %a1
    cmp.l   #7, %d0
    bne.s   f3
    bsr     pass
    bra.s   t4
f3: bsr     fail

t4: | 4. FADD.X #imm
    fmove.l #0, %fp0
    .word   0xf23c, 0x4822      | FADD.X #<ext imm>,FP0
    .long   0x40000000, 0xa0000000, 0x00000000   | 2.5 (拡張精度)
    fmove.l %fp0, %d0
    move.l  %d0, %d6
    lea     m4(%pc), %a1
    cmp.l   #2, %d0             | 2.5 を最近接偶数へ丸めて 2
    bne.s   f4
    bsr     pass
    bra.s   done
f4: bsr     fail

done:
    .word   0xff00              | DOS _EXIT

| FP0 を拡張精度で取り出し 1.0 と完全一致なら Z=1。D6 = 先頭ロング(NG 表示用)
isone:
    lea     xbuf(%pc), %a2
    fmove.x %fp0, (%a2)
    move.l  (%a2), %d6
    cmp.l   #0x3fff0000, (%a2)+
    bne.s   1f
    cmp.l   #0x80000000, (%a2)+
    bne.s   1f
    tst.l   (%a2)
1:  rts

| 拡張精度の実測値の仮数(2ロング)を表示
dumpx:
    lea     xbuf(%pc), %a3
    move.l  4(%a3), %d0
    bsr     hex
    lea     xbuf(%pc), %a3
    move.l  8(%a3), %d0
    bra     hex

pass:
    move.l  %a1, -(%sp)
    .word   0xff09              | DOS _PRINT
    addq.l  #4, %sp
    pea     okm(%pc)
    .word   0xff09
    addq.l  #4, %sp
    rts
fail:
    move.l  %a1, -(%sp)
    .word   0xff09
    addq.l  #4, %sp
    pea     ngm(%pc)
    .word   0xff09
    addq.l  #4, %sp
    move.l  %d6, %d0            | 実測値を16進表示
    bra     hex

| D0 を8桁16進で表示 + 改行
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
    move.b  #13, (%a0)+
    move.b  #10, (%a0)+
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
okm: .asciz "OK\r\n"
ngm: .asciz "NG "
    .even
hbuf: .space 16
xbuf: .space 12
