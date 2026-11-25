| tools-x68k/asmtest/t060cache.s -- MC68060 cache 経路(CACR/CPUSHA/CINVA)の観測
|
| スーパーバイザへ移行し、CACR を読み出して16進表示する(060turbo.sys導入後の設定値)。
| 続けて CPUSHA BC / CINVA BC を実行し、例外なく戻ることと CACR が変化しないことを確認する。
| 出力: "CACR=xxxxxxxx" / "after=xxxxxxxx" / "OK"
| 命令は未対応のアセンブラ指定を避けるため .word で直接符号化する。
|   MOVEC CACR,D0 = 4E7A 0002   CPUSHA BC = F4F8   CINVA BC = F4D8

    .text
    .globl _start
_start:
    clr.l   -(%sp)
    .word   0xff20            | DOS _SUPER (D0=旧SSP)
    addq.l  #4, %sp
    move.l  %d0, %d7          | 旧SSP を保存

    .word   0x4e7a, 0x0002    | MOVEC CACR,D0
    move.l  %d0, %d6
    pea     m1(%pc)
    .word   0xff09
    addq.l  #4, %sp
    move.l  %d6, %d0
    bsr     hex

    .word   0xf4f8            | CPUSHA BC
    .word   0xf4d8            | CINVA BC
    .word   0x4e7a, 0x0002    | MOVEC CACR,D0
    move.l  %d0, %d5
    pea     m2(%pc)
    .word   0xff09
    addq.l  #4, %sp
    move.l  %d5, %d0
    bsr     hex

    cmp.l   %d6, %d5
    bne.s   ng
    pea     okm(%pc)
    bra.s   out
ng:
    pea     ngm(%pc)
out:
    .word   0xff09
    addq.l  #4, %sp

    move.l  %d7, -(%sp)
    .word   0xff20            | DOS _SUPER (ユーザへ復帰)
    addq.l  #4, %sp
    .word   0xff00            | DOS _EXIT

| D0 を8桁16進で表示 + 改行
hex:
    lea     hbuf+8(%pc), %a0
    clr.b   (%a0)
    move.b  #10, -(%a0)
    move.b  #13, -(%a0)
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
m1: .asciz "CACR="
m2: .asciz "after="
okm: .asciz "OK\r\n"
ngm: .asciz "NG\r\n"
    .even
hbuf: .space 16
