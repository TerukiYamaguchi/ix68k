| MC68030 cache/wait Tier 3 benchmark (x68000 Issue #205).
|
| Build:
|   ASMTEST_CPU=68030 tools-x68k/asmtest/build.sh \
|       tools-x68k/asmtest/cache_tier3_bench.s disk/WORK.XDF
|
| The result order is wait (00,04,0A), CACR (0101,0001,0100,0000),
| benchmark (B1..B8): 3 * 4 * 8 = 96 longword Timer-C tick counts.  At the
| READY prompt A5 points at this 384-byte array, so MCP MEM PEEK can capture
| every row without relying on the 31-line console scrollback.  Pressing a
| key prints CSV and exits normally.
|
| Timer C runs at the 4 MHz MFP clock divided by 200 (one tick = 50 us) with
| the Human68k reload of 200.  During a measurement only the Timer C
| interrupt is unmasked (IMRA = 0, IMRB = Timer C, SR level 5); its handler
| counts reloads, so a row is reloads * 200 + (200 - TCDR) ticks and is not
| limited to one 8-bit period.  The handler runs once per 10 ms.
|
| Before the READY prompt the rows are also passed to IOCS _ONTIME: first
| D1 = $54330060 ('T3', 96 rows), then D1 = RR_TTTTTT per row (RR = row
| 00..5f, TTTTTT = ticks), so an IOCS trace of a reference emulator records
| the same values without console capture.

    .equ MFP_GPIP,       0xE88001
    .equ MFP_IERB,       0xE88009
    .equ MFP_IPRB,       0xE8800D
    .equ MFP_ISRB,       0xE8800F
    .equ MFP_IMRA,       0xE88013
    .equ MFP_IMRB,       0xE88015
    .equ MFP_VR,         0xE88017
    .equ MFP_TCDCR,      0xE8801D
    .equ MFP_TCDR,       0xE88023
    .equ WAIT_REG,       0xE8E009
    .equ CRTC_R21,       0xE8002A
    .equ TVRAM_CODE,     0xE7F000     | plane 3, raster 992: off screen
    .equ ROM_RTS,        0xFE0038

    .equ TC_BIT,         0x20         | IERB/IPRB/ISRB/IMRB bit 5
    .equ TC_CLEAR,       0xDF         | write-0-to-clear mask for bit 5
    .equ TC_VECTOR_LOW,  5            | vector = VR[7:4] | 5
    .equ TC_PRESCALE200, 0x70         | TCDCR bits 6-4
    .equ TC_RELOAD,      200
    .equ SR_TIMER_ONLY,  0x2500       | MFP (level 6) and NMI only
    .equ SR_NO_IRQ,      0x2700
    .equ CACR_CI_CD,     0x00000808   | clear I-cache (bit 3), D-cache (bit 11)

    .equ B1_REP,         13
    .equ B1_ITER,        6000
    .equ B2_REP,         24
    .equ B2_ITER,        32
    .equ B3_REP,         14
    .equ B3_ITER,        2000
    .equ B4_REP,         36
    .equ B4_ITER,        128
    .equ B5_REP,         56
    .equ B5_ITER,        128
    .equ B6_REP,         16
    .equ B6_ITER,        4096
    .equ B7_REP,         20
    .equ B7_ITER,        512
    .equ B8_REP,         20
    .equ B8_ITER,        4096
    .equ BENCH_COUNT,    8
    .equ CACHE_COUNT,    4
    .equ WAIT_COUNT,     3
    .equ RESULT_COUNT,   96
    .equ TRACE_HEADER,   0x54330060   | 'T3', 96 rows
    .equ IOCS_ONTIME,    0x7f

    .text
    .globl _start
_start:
    suba.l  %a1, %a1
    move.w  #0x81, %d0           | IOCS _B_SUPER
    trap    #15
    move.l  %d0, old_ssp

    move.w  %sr, saved_sr
    ori.w   #0x0700, %sr         | nothing may run while MFP/R21 are changed
    move.b  MFP_TCDCR, saved_tcdcr
    move.b  MFP_IERB, saved_ierb
    move.b  MFP_IMRA, saved_imra
    move.b  MFP_IMRB, saved_imrb
    move.b  WAIT_REG, saved_wait
    .word   0x4e7a, 0x0002       | MOVEC CACR,D0
    move.l  %d0, saved_cacr
    move.w  CRTC_R21, saved_r21

    | Install the reload counter on the Timer C vector.
    moveq   #0, %d0
    move.b  MFP_VR, %d0
    andi.b  #0xf0, %d0
    ori.b   #TC_VECTOR_LOW, %d0
    lsl.w   #2, %d0
    movea.w %d0, %a0
    move.l  %a0, tc_vector_addr
    move.l  (%a0), saved_tc_vector
    move.l  #tc_handler, (%a0)

    | Execute B6 from non-cacheable TVRAM so that CACR does not change its
    | instruction fetches either; only the MFP read pattern is measured.
    | GVRAM is unusable for code: its word width depends on the screen mode.
    clr.w   CRTC_R21             | plain single-plane TVRAM writes
    lea     b6_blob, %a0
    lea     TVRAM_CODE, %a1
    move.w  #(b6_blob_end-b6_blob)/2-1, %d0
copy_b6:
    move.w  (%a0)+, (%a1)+
    dbra    %d0, copy_b6

    clr.b   MFP_IMRA
    move.b  #TC_BIT, MFP_IMRB
    ori.b   #TC_BIT, MFP_IERB

    clr.w   wait_index
    lea     results, %a4
wait_loop:
    move.w  wait_index, %d0
    lea     wait_values, %a0
    move.b  (%a0,%d0.w), %d1
    move.b  %d1, WAIT_REG
    clr.w   cache_index
cache_loop:
    move.w  cache_index, %d0
    lsl.w   #2, %d0
    lea     cache_values, %a0
    move.l  (%a0,%d0.w), %d0
    move.l  %d0, current_cacr
    or.l    #CACR_CI_CD, %d0     | invalidate once per CACR setting, before B1
    .word   0x4e7b, 0x0002       | MOVEC D0,CACR
    move.l  current_cacr, %d0
    .word   0x4e7b, 0x0002       | MOVEC D0,CACR
    clr.w   bench_index
bench_loop:
    move.w  bench_index, %d0
    lsl.w   #2, %d0
    lea     bench_table, %a0
    movea.l (%a0,%d0.w), %a1
    bsr     measure_case
    move.l  %d0, (%a4)+
    addq.w  #1, bench_index
    cmpi.w  #BENCH_COUNT, bench_index
    bne     bench_loop
    addq.w  #1, cache_index
    cmpi.w  #CACHE_COUNT, cache_index
    bne     cache_loop
    addq.w  #1, wait_index
    cmpi.w  #WAIT_COUNT, wait_index
    bne     wait_loop

    move.l  saved_cacr, %d0
    .word   0x4e7b, 0x0002       | restore CACR
    move.b  saved_wait, WAIT_REG
    move.b  saved_tcdcr, %d0
    andi.b  #0x07, %d0
    move.b  %d0, MFP_TCDCR
    move.b  #TC_RELOAD, MFP_TCDR | Human68k standard Timer-C reload
    move.b  #TC_CLEAR, MFP_IPRB
    movea.l tc_vector_addr, %a0
    move.l  saved_tc_vector, (%a0)
    move.b  saved_ierb, MFP_IERB
    move.b  saved_imra, MFP_IMRA
    move.b  saved_imrb, MFP_IMRB
    move.b  saved_tcdcr, MFP_TCDCR
    move.w  saved_r21, CRTC_R21
    move.w  saved_sr, %sr

    movea.l old_ssp, %a1
    move.w  #0x81, %d0           | return to user mode
    trap    #15
    bsr     emit_trace
    lea     ready_message, %a1
    move.w  #0x21, %d0
    trap    #15

    | A5 is a stable MCP-readable result pointer while this normal key-polled
    | exit path waits.
    lea     results, %a5
key_wait:
    move.w  #0x01, %d0           | IOCS _B_KEYSNS
    trap    #15
    tst.l   %d0
    beq     key_wait
    move.w  #0x00, %d0           | IOCS _B_KEYINP
    trap    #15
    bsr     print_csv
    .word   0xff00                | DOS _EXIT

tc_handler:
    addq.l  #1, tc_reloads
    move.b  #TC_CLEAR, MFP_ISRB  | harmless when VR selects automatic EOI
    rte

| D0.l = elapsed Timer-C ticks, A1 = benchmark routine.  A benchmark may
| clobber D0-D7/A0-A3/A5-A6; A4 is saved outside the timed window.
measure_case:
    move.l  %a4, -(%sp)
    move.b  saved_tcdcr, %d2
    andi.b  #0x07, %d2
    move.b  %d2, MFP_TCDCR       | stop Timer C, keep Timer D
    move.b  #TC_RELOAD, MFP_TCDR
    move.b  #TC_CLEAR, MFP_IPRB
    move.b  #TC_CLEAR, MFP_ISRB
    clr.l   tc_reloads
    ori.b   #TC_PRESCALE200, %d2
    move.b  %d2, MFP_TCDCR
    move.w  #SR_TIMER_ONLY, %sr
    jsr     (%a1)
    move.w  #SR_NO_IRQ, %sr
    move.b  saved_tcdcr, %d2
    andi.b  #0x07, %d2
    move.b  %d2, MFP_TCDCR
    moveq   #0, %d1
    move.b  MFP_TCDR, %d1
    move.l  tc_reloads, %d0
    btst    #5, MFP_IPRB         | a reload not yet serviced
    beq     1f
    addq.l  #1, %d0
1:
    move.b  #TC_CLEAR, MFP_IPRB
    mulu.w  #TC_RELOAD, %d0
    addi.l  #TC_RELOAD, %d0
    sub.l   %d1, %d0
    movea.l (%sp)+, %a4
    rts

| B1: RAM-resident loop body below 256 bytes.
b1_case:
    move.w  #B1_REP-1, %d6
b1_rep:
    move.w  #B1_ITER-1, %d0
b1_loop:
    nop
    dbra    %d0, b1_loop
    dbra    %d6, b1_rep
    rts

| B2: RAM-resident execution span strictly larger than 512 bytes.
b2_case:
    move.w  #B2_REP-1, %d6
b2_rep:
    move.w  #B2_ITER-1, %d0
b2_loop:
    .rept 260
    nop
    .endr
    dbra    %d0, b2_loop
    dbra    %d6, b2_rep
    rts

| B3 ROM-fetch proxy.  A .X cannot place its loop in ROM, so the RAM driver
| executes the RTS at IPLROM30 $FE0038 each iteration.  This pins the ROM
| image rather than guessing an entry point:
| SHA-256 bdba942ab9c633a3172fbf1a8579849df52c0eeb0da8a3411402f4564d035a27.
b3_case:
    move.w  #B3_REP-1, %d6
b3_rep:
    move.w  #B3_ITER-1, %d0
b3_loop:
    jsr     ROM_RTS
    dbra    %d0, b3_loop
    dbra    %d6, b3_rep
    rts

| B4/B5 repeatedly traverse a 128-byte D-cache working set.
b4_case:
    move.w  #B4_REP-1, %d6
b4_rep:
    move.w  #B4_ITER-1, %d1
b4_outer:
    lea     b4_data, %a0
    .rept 32
    move.l  (%a0)+, %d0
    .endr
    dbra    %d1, b4_outer
    dbra    %d6, b4_rep
    rts

b5_case:
    move.l  #0x55aa33cc, %d0
    move.w  #B5_REP-1, %d6
b5_rep:
    move.w  #B5_ITER-1, %d1
b5_outer:
    lea     b5_data, %a0
    .rept 32
    move.l  %d0, (%a0)+
    .endr
    dbra    %d1, b5_outer
    dbra    %d6, b5_rep
    rts

| B6 is copied to TVRAM_CODE; it must stay position independent.
b6_blob:
    move.w  #B6_REP-1, %d6
b6_rep:
    move.w  #B6_ITER-1, %d1
b6_loop:
    move.b  MFP_GPIP, %d0
    dbra    %d1, b6_loop
    dbra    %d6, b6_rep
    rts
b6_blob_end:

b7_case:
    move.w  #B7_REP-1, %d6
b7_rep:
    move.w  #B7_ITER-1, %d7
b7_loop:
    lea     b7_data, %a0
    movem.l (%a0), %d0-%d5/%a1-%a6
    dbra    %d7, b7_loop
    dbra    %d6, b7_rep
    rts

b8_case:
    lea     b8_data, %a0
    move.w  #B8_REP-1, %d6
b8_rep:
    move.w  #B8_ITER-1, %d0
b8_loop:
    tas     (%a0)
    dbra    %d0, b8_loop
    dbra    %d6, b8_rep
    rts

| One harmless IOCS _ONTIME call per row; D1 carries row and ticks.
emit_trace:
    move.l  #TRACE_HEADER, %d1
    move.w  #IOCS_ONTIME, %d0
    trap    #15
    lea     results, %a4
    moveq   #0, %d3
emit_loop:
    move.l  (%a4)+, %d1
    andi.l  #0x00ffffff, %d1
    move.l  %d3, %d0
    ror.l   #8, %d0
    or.l    %d0, %d1
    move.w  #IOCS_ONTIME, %d0
    trap    #15
    addq.w  #1, %d3
    cmpi.w  #RESULT_COUNT, %d3
    bne     emit_loop
    rts

| Console CSV mirrors the MCP result array.  ticks_hex * 50 = microseconds.
print_csv:
    lea     csv_header, %a1
    move.w  #0x21, %d0
    trap    #15
    clr.w   wait_index
    lea     results, %a4
print_wait:
    clr.w   cache_index
print_cache:
    clr.w   bench_index
print_bench:
    lea     csv_line, %a0
    move.w  bench_index, %d0
    addi.b  #'1', %d0
    move.b  %d0, 1(%a0)
    move.w  cache_index, %d0
    lsl.w   #2, %d0
    lea     cache_values, %a1
    move.l  (%a1,%d0.w), %d0
    lea     3(%a0), %a1
    bsr     hex16
    move.w  wait_index, %d0
    lea     wait_values, %a1
    move.b  (%a1,%d0.w), %d0
    lea     8(%a0), %a1
    bsr     hex8
    move.w  bench_index, %d0
    lsl.w   #2, %d0
    lea     loop_counts, %a1
    move.l  (%a1,%d0.w), %d0
    swap    %d0
    lea     11(%a0), %a1
    bsr     hex16
    swap    %d0
    lea     16(%a0), %a1
    bsr     hex16
    move.l  (%a4)+, %d0
    swap    %d0
    lea     21(%a0), %a1
    bsr     hex16
    swap    %d0
    lea     25(%a0), %a1
    bsr     hex16
    lea     csv_line, %a1
    move.w  #0x21, %d0
    trap    #15
    addq.w  #1, bench_index
    cmpi.w  #BENCH_COUNT, bench_index
    bne     print_bench
    addq.w  #1, cache_index
    cmpi.w  #CACHE_COUNT, cache_index
    bne     print_cache
    addq.w  #1, wait_index
    cmpi.w  #WAIT_COUNT, wait_index
    bne     print_wait
    rts

| Writes D0.b as two hex digits to (A1)+.  D0 is preserved.
hex8:
    movem.l %d0-%d1/%a0, -(%sp)
    lea     hex_digits, %a0
    move.b  %d0, %d1
    lsr.b   #4, %d1
    andi.w  #0x000f, %d1
    move.b  (%a0,%d1.w), (%a1)+
    andi.w  #0x000f, %d0
    move.b  (%a0,%d0.w), (%a1)+
    movem.l (%sp)+, %d0-%d1/%a0
    rts

| Writes D0.w as four hex digits to (A1)+.  D0 is preserved.
hex16:
    movem.l %d0-%d2/%a0, -(%sp)
    lea     hex_digits, %a0
    moveq   #3, %d2
hex16_loop:
    rol.w   #4, %d0
    move.w  %d0, %d1
    andi.w  #0x000f, %d1
    move.b  (%a0,%d1.w), (%a1)+
    dbra    %d2, hex16_loop
    movem.l (%sp)+, %d0-%d2/%a0
    rts

    .data
    .align 2
bench_table:
    .long b1_case, b2_case, b3_case, b4_case
    .long b5_case, TVRAM_CODE, b7_case, b8_case
cache_values:
    .long 0x00000101, 0x00000001, 0x00000100, 0x00000000
| High word = outer repetitions, low word = inner iterations.
loop_counts:
    .word B1_REP, B1_ITER, B2_REP, B2_ITER, B3_REP, B3_ITER, B4_REP, B4_ITER
    .word B5_REP, B5_ITER, B6_REP, B6_ITER, B7_REP, B7_ITER, B8_REP, B8_ITER
wait_values:
    .byte 0x00, 0x04, 0x0a
hex_digits:
    .ascii "0123456789ABCDEF"
ready_message:
    .asciz "TIER3 READY ROWS=96 LONGS=96 ORDER=W,C,B; A5=results; press a key for CSV.\r\n"
csv_header:
    .asciz "bench,cacr,wait,rep,iter,ticks_hex(50us)\r\n"
csv_line:
    .ascii "B0,0000,00,0000,0000,00000000\r\n"
    .byte 0

    .bss
    .align 4
results:
    .space RESULT_COUNT*4
b4_data:
    .space 128
b5_data:
    .space 128
b7_data:
    .space 48
b8_data:
    .space 1
    .align 4
tc_reloads:
    .space 4
tc_vector_addr:
    .space 4
saved_tc_vector:
    .space 4
old_ssp:
    .space 4
saved_cacr:
    .space 4
current_cacr:
    .space 4
saved_sr:
    .space 2
saved_r21:
    .space 2
wait_index:
    .space 2
cache_index:
    .space 2
bench_index:
    .space 2
saved_tcdcr:
    .space 1
saved_ierb:
    .space 1
saved_imra:
    .space 1
saved_imrb:
    .space 1
saved_wait:
    .space 1
