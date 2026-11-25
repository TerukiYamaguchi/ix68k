/* Human68k device driver header (loopdrv-x68k の head.S を参考に構成) */

    .text

    .extern reqheader
    .extern interrupt
    .global hostdrv_b_print
    .global hostdrv_probe_magic

devheader:
    .long   -1                  // link pointer
    .word   0x2000              // device attribute: block device + remote (bit13)
    .long   strategy            // strategy routine entry point
    .long   interrupt_asm       // interrupt routine entry point
    .ascii  "\x01HOSTDRV"        // ユニット数(1バイト) + ドライバ名(7文字)

strategy:
    move.l  %a5,reqheader
    rts

interrupt_asm:
    movem.l %d1-%d2/%a0-%a2/%a5,%sp@-
    bsr     interrupt
    movem.l %sp@+,%d1-%d2/%a0-%a2/%a5
    rts

/* A1 に渡した Shift_JIS の NUL 終端文字列を IOCS _B_PRINT で表示する。
 * 常駐ドライバは標準 C ライブラリをリンクしないため、TRAP #15 を直接使う。 */
hostdrv_b_print:
    move.l  %sp@(4),%a1
    move.w  #0x21,%d0
    trap    #15
    rts

/* $EA0200 の MAGIC ワードをバスエラー保護付きで読む。戻り値は d0:
 *   0x0000xxxx = 読めた値、0xFFFFFFFF = バスエラー（ix68k 以外では $EA0200 が未割り当て）。
 * IOCS _B_WPEEK は保護なしの move.w (a1)+,d0 なので使えない。バスエラーベクタ($08)を
 * 一時的に差し替え、発生時は CPU 種別ごとに異なる例外フレームを sp の復元で捨てる。
 * ROM の _B_INTVCS と同様にベクタは絶対アドレス $08 を直接使う。
 * ドライバ初期化はスーパーバイザ状態で動くため SR を直接操作できる。 */
hostdrv_probe_magic:
    movem.l %d2/%a2,%sp@-
    move.w  %sr,%d2
    ori.w   #0x0700,%sr
    movea.l 0x8:w,%a2
    move.l  #probe_fault,0x8:w
    move.l  %sp,probe_sp
    moveq   #0,%d0
    move.w  0xEA0200,%d0
probe_done:
    move.l  %a2,0x8:w
    move.w  %d2,%sr
    movem.l %sp@+,%d2/%a2
    rts

probe_fault:
    move.l  probe_sp,%sp
    moveq   #-1,%d0
    bra     probe_done

    .data
probe_sp:
    .long   0

    .end
