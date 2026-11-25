| tools-x68k/asmtest/wr_del_test.s -- IOCS _B_WRITED(0x49)/_B_READDL(0x4C)
| 動作検証(Issue #166設計書 §5)。FD1(B:, PDA=$91)へDDAM(削除データ)ビットを
| 立てたり消したりしながらREAD/WRITE/READDL/WRITEDの組み合わせを実行し、
| 結果を固定RAMアドレスへ並べて保存する。
|
| D1.w = PDA<<8 | flags($70=MFM+retry+seek)
| D2.l = N<<24 | C<<16 | H<<8 | R
| D3.l = バイト数, A1 = バッファ
|
| 結果レイアウト($0FF000起点、各4バイトD0 + 1バイトmismatchフラグ):
|   +0x00: (1) _B_WRITED C10H0R1 pattern1 -> D0
|   +0x04: (2) _B_READDL 同セクタ -> D0 (CM無し期待)
|   +0x08: (2m) 読み出しデータがpattern1と不一致なら1
|   +0x0C: (3) _B_READ(0x46) 同セクタ(DDAM=1のまま) -> D0 (CM期待、リトライ後)
|   +0x10: (3m) 読み出しデータがpattern1と不一致なら1
|   +0x14: (4) _B_WRITE(0x45) 同セクタ pattern2 -> D0 (DDAMクリア)
|   +0x18: (5) _B_READDL 同セクタ -> D0 (CM期待、通常データになった)
|   +0x1C: (5m) 読み出しデータがpattern2と不一致なら1
|   +0x20: (6) _B_READDL C11H0R1(未触sector) -> D0 (CM期待)
|   +0x24: (7) _B_WRITED C77H0R1 -> D0 (ND期待、境界値)
|   +0x28: (8) _B_WRITED C10H0R0 -> D0 (ND期待)
|   +0x2C: (9) _B_WRITED C10H0R9 -> D0 (ND期待)
|   +0x30: (10) _B_READDL C77H0R1 -> D0 (ND期待)
|   +0x34: (11) _B_WRITED PDA=$80(SASI,未接続) -> D0 (-1期待)
|   +0x38: (12) _B_READDL PDA=$80 -> D0 (-1期待)
|   +0x3C: (13) 境界値テスト後もC10H0R1のデータがpattern2のままか(不一致なら1)

    .text
    .globl  _start
_start:
    lea     0x0FF000, %a3       | 結果ポインタ

    | ---- pattern1(0..1023を1バイトにキャストした値)をバッファへ用意 ----
    lea     0x0F0000, %a2
    move.w  #1023, %d5
fill_p1:
    move.w  %d5, %d4
    move.b  %d4, (%a2)+
    dbra    %d5, fill_p1

    | ---- (1) _B_WRITED C10H0R1 pattern1 ----
    move.w  #0x49, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- 読み出しバッファをクリア(前回データの残留を排除) ----
    lea     0x0F1000, %a2
    move.w  #1023, %d5
clear_r1:
    clr.b   (%a2)+
    dbra    %d5, clear_r1

    | ---- (2) _B_READDL 同セクタ ----
    move.w  #0x4C, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    move.l  %d0, (%a3)+
    bsr     cmp_p1_r1
    move.l  %d0, (%a3)+

    | ---- 読み出しバッファをクリア ----
    lea     0x0F1000, %a2
    move.w  #1023, %d5
clear_r2:
    clr.b   (%a2)+
    dbra    %d5, clear_r2

    | ---- (3) _B_READ(0x46) 同セクタ(DDAM=1のまま) ----
    move.w  #0x46, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    move.l  %d0, (%a3)+
    bsr     cmp_p1_r1
    move.l  %d0, (%a3)+

    | ---- pattern2(反転)をバッファへ用意 ----
    lea     0x0F0400, %a2
    move.w  #1023, %d5
fill_p2:
    move.w  %d5, %d4
    not.b   %d4
    move.b  %d4, (%a2)+
    dbra    %d5, fill_p2

    | ---- (4) _B_WRITE(0x45) 同セクタ pattern2 ----
    move.w  #0x45, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F0400, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- 読み出しバッファをクリア ----
    lea     0x0F1000, %a2
    move.w  #1023, %d5
clear_r3:
    clr.b   (%a2)+
    dbra    %d5, clear_r3

    | ---- (5) _B_READDL 同セクタ(通常データになった) ----
    move.w  #0x4C, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    move.l  %d0, (%a3)+
    bsr     cmp_p2_r1
    move.l  %d0, (%a3)+

    | ---- (6) _B_READDL C11H0R1(未触sector) ----
    move.w  #0x4C, %d0
    move.w  #0x9170, %d1
    move.l  #0x030B0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (7) _B_WRITED C77H0R1(境界値) ----
    move.w  #0x49, %d0
    move.w  #0x9170, %d1
    move.l  #0x034D0001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (8) _B_WRITED C10H0R0(境界値) ----
    move.w  #0x49, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0000, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (9) _B_WRITED C10H0R9(境界値) ----
    move.w  #0x49, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0009, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (10) _B_READDL C77H0R1(境界値) ----
    move.w  #0x4C, %d0
    move.w  #0x9170, %d1
    move.l  #0x034D0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (11) _B_WRITED PDA=$80(SASI, 未接続) ----
    move.w  #0x49, %d0
    move.w  #0x8070, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F0000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (12) _B_READDL PDA=$80 ----
    move.w  #0x4C, %d0
    move.w  #0x8070, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    move.l  %d0, (%a3)+

    | ---- (13) 境界値テスト後もC10H0R1のデータがpattern2のままか ----
    lea     0x0F1000, %a2
    move.w  #1023, %d5
clear_r4:
    clr.b   (%a2)+
    dbra    %d5, clear_r4
    move.w  #0x46, %d0
    move.w  #0x9170, %d1
    move.l  #0x030A0001, %d2
    move.l  #1024, %d3
    lea     0x0F1000, %a1
    trap    #15
    bsr     cmp_p2_r1
    move.l  %d0, (%a3)+

    .word   0xff00              | DOS _EXIT

| cmp_p1_r1: $0F0000(pattern1) と $0F1000(読み出し結果) を1024バイト比較。
| 不一致があれば D0=1、一致なら D0=0。
cmp_p1_r1:
    movem.l %a0/%a1/%d1, -(%a7)
    lea     0x0F0000, %a0
    lea     0x0F1000, %a1
    move.w  #1023, %d1
    moveq   #0, %d0
cmp1_loop:
    cmpm.b  (%a0)+, (%a1)+
    bne     cmp1_mismatch
    dbra    %d1, cmp1_loop
    bra     cmp1_done
cmp1_mismatch:
    moveq   #1, %d0
cmp1_done:
    movem.l (%a7)+, %a0/%a1/%d1
    rts

| cmp_p2_r1: $0F0400(pattern2) と $0F1000(読み出し結果) を1024バイト比較。
cmp_p2_r1:
    movem.l %a0/%a1/%d1, -(%a7)
    lea     0x0F0400, %a0
    lea     0x0F1000, %a1
    move.w  #1023, %d1
    moveq   #0, %d0
cmp2_loop:
    cmpm.b  (%a0)+, (%a1)+
    bne     cmp2_mismatch
    dbra    %d1, cmp2_loop
    bra     cmp2_done
cmp2_mismatch:
    moveq   #1, %d0
cmp2_done:
    movem.l (%a7)+, %a0/%a1/%d1
    rts
