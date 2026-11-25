| tools-x68k/asmtest/issue23_fault.s — x68000#92 instr_ir修正の回帰確認
| （Issue #23で使われた元のテストケース move.w d0,$200000 を再現）
|
| SWITCH M=2048（メインメモリ2MB）設定時、$200000はメモリ直後の未マップ領域。
| abs.L destinationの6バイト命令でバスエラーを起こし、Human68kダイアログの
| 表示PCが命令先頭になることを確認する。

    .text
    .globl _start
_start:
    move.w  #0, %d0          | 明示的にCCRを既知の状態にしておく(Z=1)
issue23_fault:
    move.w  %d0, 0x200000    | abs.L destination、未マップ領域への書き込みでバスエラー
    .word   0xff00           | DOS _EXIT（上の命令が正常終了した場合のみ到達、通常は不到達）
