| tools-x68k/asmtest/t060dc.s -- MC68060 データキャッシュ ON 経路の観測
|
| IOCS _SYS_STAT(060turbo.sysが提供) でデータ/命令キャッシュを ON にし、CACR を読み出して
| 16進表示する。ON の状態でメモリの書き込み・読み出し・コピーを行い内容が一致すること、
| CPUSHA DC / CINVA DC を挟んでも一致することを確認してから元の設定へ戻す。
| 出力: "mode:WT|CB <メモリ値>"(CINVA DC で捨てた後のメモリ側の値。CB なら 0)、"pushinv:OK" も出す。
| 出力: "before=xxxxxxxx"(元のキャッシュ状態 bit1=D bit0=I) / "CACR=xxxxxxxx" / "copy:OK" / "push:OK"
|   MOVEC CACR,D0 = 4E7A 0002   CPUSHA DC = F478   CINVA DC = F458
|   _SYS_STAT = IOCS $AC, D1=4 でキャッシュ設定(D2=新状態、D0=旧状態)

    .text
    .globl _start
_start:
    clr.l   -(%sp)
    .word   0xff20              | DOS _SUPER
    addq.l  #4, %sp
    move.l  %d0, %d7            | 旧SSP

    | 元の状態を得て表示(D1=1 で取得)
    moveq   #1, %d1
    move.w  #0xAC, %d0
    trap    #15
    move.l  %d0, %d6            | 元の状態
    pea     m1(%pc)
    .word   0xff09
    addq.l  #4, %sp
    move.l  %d6, %d0
    bsr     hex

    | D/I キャッシュ ON(D1=4, D2=3)
    moveq   #4, %d1
    moveq   #3, %d2
    move.w  #0xAC, %d0
    trap    #15

    .word   0x4e7a, 0x0002      | MOVEC CACR,D0
    move.l  %d0, %d5
    pea     m2(%pc)
    .word   0xff09
    addq.l  #4, %sp
    move.l  %d5, %d0
    bsr     hex

    | バッファへパターンを書き、別バッファへコピーして比較
    lea     src(%pc), %a0
    lea     dst(%pc), %a1
    move.w  #63, %d1
    move.l  #0x13579BDF, %d2
1:  move.l  %d2, (%a0)+
    ror.l   #3, %d2
    add.l   #0x01010101, %d2
    dbra    %d1, 1b
    lea     src(%pc), %a0
    move.w  #63, %d1
2:  move.l  (%a0)+, (%a1)+
    dbra    %d1, 2b
    bsr     compare
    lea     m3(%pc), %a2
    bsr     report

    | CPUSHA DC / CINVA DC の後も一致するか。コピーバック環境でも OS の dirty line を
    | CINVA で捨てないよう、2 命令の間に割り込みを入れない(IPL=7)。
    move.w  %sr, %d4
    move.w  #0x2700, %sr
    .word   0xf478              | CPUSHA DC
    .word   0xf458              | CINVA DC
    move.w  %d4, %sr
    bsr     compare
    lea     m4(%pc), %a2
    bsr     report

    | キャッシュモード判別: 書いた直後に CINVA DC(push なし)で捨て、メモリ側の値を読む。
    | メモリが旧値 0 のままなら dirty line が捨てられた = コピーバック("CB")、
    | 書いた値が残っていれば = ライトスルー("WT")。割り込みで他の dirty line を作らないよう
    | IPL を 7 にし、先に CPUSHA DC で既存の dirty line を戻しておく。
    move.w  %sr, %d4
    move.w  #0x2700, %sr
    lea     cbuf(%pc), %a0
    clr.l   (%a0)
    .word   0xf478              | CPUSHA DC
    move.l  (%a0), %d0          | line を確保
    move.l  #0x5A5A5A5A, (%a0)
    .word   0xf458              | CINVA DC(push なし)
    move.l  (%a0), %d3
    move.w  %d4, %sr
    pea     m5(%pc)
    .word   0xff09
    addq.l  #4, %sp
    pea     wtm(%pc)
    tst.l   %d3
    bne.s   5f
    addq.l  #4, %sp
    pea     cbm(%pc)
5:  .word   0xff09
    addq.l  #4, %sp
    move.l  %d3, %d0
    bsr     hex

    | CPUSHA DC の後なら CINVA DC を挟んでも値が残る
    move.w  #0x2700, %sr
    lea     cbuf(%pc), %a0
    move.l  #0xA5A5A5A5, (%a0)
    .word   0xf478              | CPUSHA DC
    .word   0xf458              | CINVA DC
    cmp.l   #0xA5A5A5A5, (%a0)
    seq     %d0
    move.w  %d4, %sr
    lea     m6(%pc), %a2
    bsr     report

    | 元の状態へ戻す
    moveq   #4, %d1
    move.l  %d6, %d2
    move.w  #0xAC, %d0
    trap    #15

    move.l  %d7, -(%sp)
    .word   0xff20              | DOS _SUPER(ユーザへ復帰)
    addq.l  #4, %sp
    .word   0xff00

| src と dst を比較。D0=0 なら一致
compare:
    lea     src(%pc), %a0
    lea     dst(%pc), %a1
    move.w  #63, %d1
1:  cmpm.l  (%a0)+, (%a1)+
    dbne    %d1, 1b
    seq     %d0
    rts

| D0.b=$FF なら "<A2>OK" さもなくば "<A2>NG"
report:
    move.b  %d0, -(%sp)
    move.l  %a2, -(%sp)
    .word   0xff09
    addq.l  #4, %sp
    tst.b   (%sp)+
    beq.s   3f
    pea     okm(%pc)
    bra.s   4f
3:  pea     ngm(%pc)
4:  .word   0xff09
    addq.l  #4, %sp
    rts

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
m1: .asciz "before="
m2: .asciz "CACR="
m3: .asciz "copy:"
m4: .asciz "push:"
m5: .asciz "mode:"
m6: .asciz "pushinv:"
wtm: .asciz "WT "
cbm: .asciz "CB "
okm: .asciz "OK\r\n"
ngm: .asciz "NG\r\n"
    .even
hbuf: .space 16
    .balign 16
cbuf: .space 16
src: .space 256
dst: .space 256
