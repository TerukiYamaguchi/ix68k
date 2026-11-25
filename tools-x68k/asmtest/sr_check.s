| tools-x68k/asmtest/sr_check.s — GVRAM書き込み直後にループし、SR/PCを確認する診断用
    .text
    .globl _start
_start:
    move.w  #0, %d0
    move.w  %d0, 0xC00000    | GVRAM書き込み。保護有効時、ユーザモードならバスエラー
loop:
    bra     loop             | 無限ループで停止し、get_registersでSR/PCを確認する
