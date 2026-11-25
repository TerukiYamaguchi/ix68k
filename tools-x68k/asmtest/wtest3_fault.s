| tools-x68k/asmtest/wtest3_fault.s — x68000#92: バスエラー例外PCがXEiJと一致するか確認
|
| mc68000側Issue #10の修正（dispatch_group0_exception()がcpu->pcでなく
| cpu->instr_pc＝命令先頭をスタックする）を、Human68kのバスエラーダイアログの
| 表示PCで検証する。abs.L destinationへの書き込みは拡張ワードを持つため、
| 命令先頭とフェッチ完了後PCがずれる。
|
| 書き込み先 0xEE0000 は src/bus/x68k_bus.c の addr_in_bus_error_gap()
| コメントに「$EE0000-$EFFFFF: 未使用（インサイドX68000 図1参照）」と明記
| された、SRAMのメモリ搭載量設定に関わらず常に未マップな領域。

    .text
    .globl _start
_start:
    move.w  #0, %d0          | 明示的にCCRを既知の状態にしておく(Z=1)
wtest3_fault:
    move.w  %d0, 0xee0000    | abs.L destination、未マップ領域への書き込みでバスエラー
    .word   0xff00           | DOS _EXIT（上の命令が正常終了した場合のみ到達、通常は不到達）
