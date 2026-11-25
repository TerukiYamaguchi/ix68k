| tools-x68k/asmtest/rs232_e2e_test.s -- RS-232C双方向E2Eテスト（Issue #48 Phase 4）
|
| 起動時に固定文字列を_OUT232Cで送信し、その後は_ISNS232C/_INP232Cで受信データを
| ポーリングして_B_PUTCで画面表示し続ける。ESCキーで終了する。
| ホスト側は/dev/ttysXXX(PTYスレーブ)へ書き込むことで受信テストができる。

    .text
    .globl _start
_start:
    move.w  #-1, %d1          | _SET232C: -1=現在設定を照会するのみ(変更しない)
    move.w  #0x30, %d0
    trap    #15

    lea     msg, %a0
send_char:
    move.b  (%a0)+, %d1
    beq     recv_loop         | ヌル終端で送信完了、受信ループへ
wait_tx_ready:
    move.w  #0x34, %d0        | _OSNS232C: 0=送信不可 4=送信可能
    trap    #15
    tst.l   %d0
    beq     wait_tx_ready
    move.w  #0x35, %d0        | _OUT232C (D1.Bは直前のmove.bで設定済み)
    trap    #15
    bra     send_char

recv_loop:
    move.w  #0x01, %d0        | _B_KEYSNS: ESC押下で終了
    trap    #15
    tst.l   %d0
    beq     check_rs232
    cmp.b   #0x1B, %d0
    beq     exit_prog
    move.w  #0x00, %d0        | _B_KEYINP: バッファを空にする(ESC以外は読み捨て)
    trap    #15

check_rs232:
    move.w  #0x33, %d0        | _ISNS232C: 0=データなし $100??=あり(下位バイト受信データ)
    trap    #15
    tst.l   %d0
    beq     recv_loop
    move.w  #0x32, %d0        | _INP232C: 受信データ取得(D0.Wの下位8bit)
    trap    #15
    move.w  %d0, %d1
    move.w  #0x20, %d0        | _B_PUTC: D1.W=表示文字コード
    trap    #15
    bra     recv_loop

exit_prog:
    .word   0xff00             | DOS _EXIT

msg:
    .ascii  "X68K RS232 TEST\r\n"
    .byte   0
