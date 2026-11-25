| tools-x68k/asmtest/datetime_cnv_test.s -- _DATEASC/_TIMEASC/_DAYASC/_DATECNV/_TIMECNV
| (0x5A/0x5B/0x5C/0x58/0x59) ROM委譲の動作検証
|
| 結果をRAM上の固定アドレスへ書き出し、MCP read_memoryで確認する。
| 0xF8000: DATEASC出力文字列("yyyy/mm/dd",0想定)
| 0xF8020: TIMEASC出力文字列("hh:mm:ss",0想定)
| 0xF8040: DAYASC出力文字列(曜日名、3バイト以上)
| 0xF8100: 各D0返値(long x5、DATEASC/TIMEASC/DAYASC/DATECNV/TIMECNVの順)

    .text
    .globl _start
_start:
    | DATEASC(0x5A): 2026/07/13, format=0('yyyy/mm/dd')
    move.l  #0x07EA070D, %d1
    lea     0xF8000, %a1
    move.w  #0x5A, %d0
    trap    #15
    move.l  %d0, 0xF8100

    | TIMEASC(0x5B): 12:34:56
    move.l  #0x000C2238, %d1
    lea     0xF8020, %a1
    move.w  #0x5B, %d0
    trap    #15
    move.l  %d0, 0xF8104

    | DAYASC(0x5C): 1=月曜日
    move.l  #1, %d1
    lea     0xF8040, %a1
    move.w  #0x5C, %d0
    trap    #15
    move.l  %d0, 0xF8108

    | DATECNV(0x58): "2026/07/13" -> バイナリ
    lea     datestr, %a1
    move.w  #0x58, %d0
    trap    #15
    move.l  %d0, 0xF810C

    | TIMECNV(0x59): "12:34:56" -> バイナリ
    lea     timestr, %a1
    move.w  #0x59, %d0
    trap    #15
    move.l  %d0, 0xF8110

    .word   0xff00            | DOS _EXIT

datestr:
    .asciz  "2026/07/13"
timestr:
    .asciz  "12:34:56"
