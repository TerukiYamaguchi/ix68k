| tools-x68k/asmtest/super_protect_test.s
| ユーザ/スーパーバイザ メモリ保護の検証用。
| ユーザモードのまま GVRAM ($C00000) へ書き込み → 保護有効時はバスエラー期待。
| 白帯が出たら "A" で中断し、DIR等で復帰確認すること。

    .text
    .globl _start
_start:
    move.w  #0, %d0          | 明示的にCCRを既知の状態にしておく(Z=1)
    move.w  %d0, 0xC00000    | GVRAM書き込み。保護有効時、ユーザモードならバスエラー
    .word   0xff00           | DOS _EXIT（正常終了した場合のみ到達）
