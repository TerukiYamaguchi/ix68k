| tools-x68k/asmtest/template.s — 使い捨てテストプログラムのひな型
|
| GNU as (m68k-xelf-as) 構文の注意点:
|   - レジスタは %d0/%a0 のように % 接頭辞
|   - 即値は #imm、絶対アドレスはそのまま数値（$ 接頭辞は付けない）
|   - move.w %d0, 0x200000       … MOVE.W D0,(abs.L)
|   - move.w 0x200000, %d0       … MOVE.W (abs.L),D0
|   - DOS コールは実命令ではなく「Line-F (行1111) 命令」= $FFxx のワードを
|     直接埋め込むことで発生する（Human68k はこれを vector 11 でトラップして
|     解釈する）。.word 0xff00 が DOS _EXIT。関数コード一覧は
|     Human68k のプログラマーズマニュアルを参照。
|   - IOCS (TRAP #15) を使いたい場合は `trap #15` の前に d0 へファンクション
|     番号を入れる（doscall.mac 相当のマクロはこの環境には無いので手書きする）。

    .text
    .globl _start
_start:
    move.w  #0, %d0          | 明示的にCCRを既知の状態にしておく(Z=1)
test_point:
    move.w  %d0, 0x200000    | ここを書き換えて検証したい命令に差し替える
    .word   0xff00           | DOS _EXIT（上の命令が正常終了した場合のみ到達）
