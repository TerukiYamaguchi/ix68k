| tools-x68k/asmtest/zerowrite_repro.s -- Issue #263 再現用
|
| 「開始クラスタ 0・サイズ 0」のエントリに対するゼロ長 DOS _WRITE で、
| Human68k 3.02 が白帯「ディスクの管理領域が破壊されています、使用不能です」を出す。
|
| GCC.X(libc の fopen("w"))の呼び出し列(2026-11-10、060turbo で採取)は次のとおり:
|     _OPEN  C:\TMP\GHOSTED.s mode=1   (既存ファイルを書込みオープン)
|     _WRITE handle, buf, len=0        (切り詰め)   <- ここで白帯
| 本プログラムはこの 2 コールだけを行う。
|
| 前提: C:\TMP\ZEROW.TMP が開始クラスタ 0・サイズ 0 のエントリであること。
|   Human68k の _CREATE は空ファイルにもクラスタを割り当てる(サイズ 0・クラスタ != 0)ので、
|   この状態は Human68k 自身では作れない(COPY NUL / TYPE NUL > / _CREATE / 切り詰めで確認)。
|   ホスト側で HDS のコピーにファイルを書き込み、エントリの開始クラスタ(+26)を 0 にする。
|   189cd14 以降の hu68disk copy は空ファイルに 1 クラスタを割り当てるため、書き換えが要る:
|       : > ZEROW.TMP
|       tools-host/hu68disk/hu68disk copy <HDS のコピー> ZEROW.TMP /TMP
|       off=$(LC_ALL=C grep -abo 'ZEROW   TMP' <HDS のコピー> | head -n 1 | cut -d: -f1)
|       printf '\000\000' | dd of=<HDS のコピー> bs=1 seek=$((off + 26)) conv=notrunc
|   必ず使い捨てのコピーで試すこと。
|
| 使い方: ZEROWRITE_REPRO を実行する(HOSTDRV 上でも C: 上でもよい)。
| 期待: "OK" を表示して終了する。
| 不具合: 白帯で停止する。FAT[0](予約値 $F7FF)を通常クラスタとして辿るため。
|
| ビルド: tools-x68k/asmtest/build.sh tools-x68k/asmtest/zerowrite_repro.s <XDF>

    .equ    OPEN_MODE_WRITE, 1
    .equ    WRITE_LEN, 0

    .text
    .globl  _start
_start:
    | ---- _OPEN mode=1 ----
    move.w  #OPEN_MODE_WRITE, -(%sp)
    pea     path(%pc)
    .word   0xff3d              | DOS _OPEN
    addq.l  #6, %sp
    tst.l   %d0
    bmi     failed
    move.w  %d0, %d6            | ファイルハンドル

    | ---- _WRITE 長さ 0 (切り詰め) ----
    move.l  #WRITE_LEN, -(%sp)
    pea     path(%pc)           | バッファは参照されない(長さ 0)
    move.w  %d6, -(%sp)
    .word   0xff40              | DOS _WRITE
    lea     10(%sp), %sp
    tst.l   %d0
    bmi     failed

    | ---- _CLOSE ----
    move.w  %d6, -(%sp)
    .word   0xff3e              | DOS _CLOSE
    addq.l  #2, %sp
    lea     msg_ok(%pc), %a0
    bra     finish

failed:
    lea     msg_ng(%pc), %a0

finish:
    move.l  %a0, -(%sp)
    .word   0xff09              | DOS _PRINT
    addq.l  #4, %sp
    .word   0xff00              | DOS _EXIT

    .data
path:
    .asciz  "C:\\TMP\\ZEROW.TMP"
msg_ok:
    .asciz  "OK\r\n"
msg_ng:
    .asciz  "DOS call failed\r\n"
