| tools-x68k/asmtest/super_protect_ok_test.s
| 対照テスト。IOCS _B_SUPER (fn=0x81) でスーパーバイザモードに
| 切り替えてから GVRAM ($C00000) へ書き込む → 保護が有効でもバスエラーが発生しない
| ことを確認する。

    .text
    .globl _start
_start:
    move.l  #0, %a1          | A1=0 → スーパーバイザモードへ切り替え
    move.w  #0x81, %d0       | _B_SUPER
    trap    #15
    move.w  #0, %d0          | 明示的にCCRを既知の状態にしておく(Z=1)
    move.w  %d0, 0xC00000    | GVRAM書き込み。スーパーバイザモードなので保護有効でも成功するはず
    .word   0xff00           | DOS _EXIT
