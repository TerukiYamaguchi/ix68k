| tools-x68k/asmtest/aiq_ask_test.s -- AIQブリッジ往復テスト
|
| 起動時に固定の "ASK <指示>\r\n" を_OUT232Cで送信し、ホスト側ブリッジ(aiq-bridge)
| からの応答 (CMD ... / END / ERR ...) を_B_PUTCで画面表示し続ける。ESCキーで終了する。

    .text
    .globl _start
_start:
    move.w  #-1, %d1          | _SET232C: -1=現在設定を照会するのみ(変更しない)
    move.w  #0x30, %d0
    trap    #15

    lea     request, %a0
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

request:
    .ascii  "ASK copy A:\\TEST.TXT to drive B:\r\n"
    .byte   0
