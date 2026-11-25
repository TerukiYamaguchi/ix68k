| tools-x68k/asmtest/addrerr_fault.s — x68000#92 Task #4: アドレスエラー（ベクタ3）でも
| instr_ir修正が機能するかの確認。dispatch_group0_exception()はバスエラー(2)と
| アドレスエラー(3)の両方を共有するため、修正は自動的に効くはずという仮説の検証。
|
| 書き込み先 0x100001 は奇数アドレス（word abs.L destinationへの書き込みで
| アドレスエラー）。既定メモリ12MB内なので未マップではなく、純粋に奇数アドレス
| 起因のアドレスエラーだけを発生させる。

    .text
    .globl _start
_start:
    move.w  #0, %d0          | 明示的にCCRを既知の状態にしておく(Z=1)
addrerr_fault:
    move.w  %d0, 0x100001    | abs.L destination、奇数アドレスへの書き込みでアドレスエラー
    .word   0xff00           | DOS _EXIT（上の命令が正常終了した場合のみ到達、通常は不到達）
