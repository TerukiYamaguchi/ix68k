| tools-x68k/asmtest/scsi_write_test.s — SCSI HDD 複数セクタ書き込みテスト
|
| カレントディレクトリに OUT.DAT を作成し、複数セクタ(4096バイト=SCSI HDDの
| 4クラスタ分、1クラスタ=1024バイト)にまたがる既知パターンを書き込んで閉じる。
| パターンは 0,1,2,...,255 の繰り返し(16回)。
|
| 使い方: XEiJ で SCSI HDD イメージ(hu68disk mkfs --type scsi で作成)をマウントし、
| そのドライブを current drive にしてこの .X を実行する。実行後、ホスト側で
| `hu68disk extract <image> /OUT.DAT out.dat` して内容を検証する。

    .text
    .globl _start
_start:
    | buffer に 0x00-0xFF の16回繰り返し(4096バイト)を書き込む
    lea     buffer, %a0
    move.w  #16-1, %d1         | 外側ループ: 16回
fill_outer:
    moveq   #0, %d0
    move.w  #256-1, %d2        | 内側ループ: 256バイト
fill_inner:
    move.b  %d0, (%a0)+
    addq.b  #1, %d0
    dbra    %d2, fill_inner
    dbra    %d1, fill_outer

    | _CREATE(FILE=filename, ATR=0x20 アーカイブ)
    move.w  #0x20, -(%sp)
    pea     filename
    .word   0xff3c              | DOS _CREATE
    addq.l  #6, %sp
    move.w  %d0, %d3            | d3.w = ファイルハンドル(負ならエラー)
    tst.l   %d0
    bmi     fail

    | _WRITE(FILENO=d3, BUFFER=buffer, LEN=4096)
    move.l  #4096, -(%sp)
    pea     buffer
    move.w  %d3, -(%sp)
    .word   0xff40              | DOS _WRITE
    lea     10(%sp), %sp

    | _CLOSE(FILENO=d3)
    move.w  %d3, -(%sp)
    .word   0xff3e              | DOS _CLOSE
    addq.l  #2, %sp

    .word   0xff00               | DOS _EXIT（正常終了）

fail:
    .word   0xff00               | 失敗時も終了（screenshot/get_registersで確認）

    .data
filename:
    .asciz  "OUT.DAT"
    .even
buffer:
    .space  4096
