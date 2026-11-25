| tools-x68k/asmtest/rs232_iocs_test.s -- IOCS 0x33(_ISNS232C)/0x34(_OSNS232C)の
| ROM委譲検証（Issue #48 Phase 3）。データ受信の有無でD0が変化することを
| 確認し、固定スタブ(常に同じ値)ではなく実ハードウェア状態を反映している
| ことを裏取りする。ブロッキングする_INP232Cはここでは呼ばない。
| 0xF8100: _ISNS232C結果(long)  0xF8104: _OSNS232C結果(long)

    .text
    .globl _start
_start:
    move.w  #0x33, %d0        | _ISNS232C: 受信データ有無
    trap    #15
    move.l  %d0, 0xF8100

    move.w  #0x34, %d0        | _OSNS232C: 送信可否
    trap    #15
    move.l  %d0, 0xF8104

    .word   0xff00            | DOS _EXIT
