| tools-x68k/asmtest/mouse_curgt_test.s — マウスカーソル座標を0x70000へ書き続ける（使い捨て）
| _MS_INIT/_MS_CURONの後、_MS_CURGT(D0.hw=X,D0.w=Y)を無限ループで呼び、
| 結果を0x70000(D0.l丸ごと)へ保存し続ける。キー入力でDOS _EXIT。
| SCCマウス移動量のオーバーフロー修正検証用（mouse_move MCPツールとの組合せ）。

    .text
    .globl _start
_start:
    move.w  #0x70, %d0
    trap    #15              | _MS_INIT

    move.l  #0, %d1          | 左端X=0, 上端Y=0
    move.l  #0x7FFF7FFF, %d2 | 右端X=0x7FFF, 下端Y=0x7FFF（クランプ回避で広く取る）
    move.w  #0x77, %d0
    trap    #15               | _MS_LIMIT

    move.w  #0x71, %d0
    trap    #15              | _MS_CURON

loop:
    move.w  #0x75, %d0
    trap    #15               | _MS_CURGT -> D0.hw=X, D0.w=Y
    move.l  %d0, 0x70000      | 上位16bit=X, 下位16bit=Y

    move.w  #0x01, %d0
    trap    #15                | _B_KEYSNS
    tst.l   %d0
    beq     loop

    .word   0xff00             | DOS _EXIT
