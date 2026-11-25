| himem_test.s — 060turbo.sys の IOCS _HIMEM($F8) 動作確認
|
| 確認内容（各項目を「OK:」「NG:」で DOS _PRINT する。最後に NG 件数を表示）
|   1. HIMEM_VERSION   d0 == '060T'
|   2. HIMEM_GETSIZE / HIMEM_GETAREA の値表示
|   3. HIMEM_MALLOC    1MB を2本確保（rc==0、a1 >= $10000000、重複なし）
|                      全域にパターンを書いて読み戻す
|   4. HIMEM_FREE      解放後に GETSIZE の合計が元に戻る
|   5. 最大ブロック    GETSIZE の max を確保し、先頭4KB/末尾4KBを書いて読み戻す
|   6. HIMEM_CALLOC    確保したブロックがゼロ初期化されている
|   7. HIMEM_REALLOC   64KB -> 256KB で内容が保持される。移動なしで済むとき a1 が
|                      ブロック先頭を返すこと（v0.56 ソースの t15memory.s realloc は、
|                      移動なしの経路で a1 に新しい末尾を返したままになっている）
|   8. 縮小 REALLOC / HIMEM_RESIZE
|
| ビルド: ASMTEST_CPU は不要（68000命令のみ）。tools-x68k/asmtest/build.sh は
|   XDF 書き込み用なので、HOSTDRV で見せる場合は README 手順のとおり .X だけ作る。

    .text
    .globl _start

    .set    OFS_TOTAL0, -4
    .set    OFS_FAILS,  -8
    .set    OFS_PA,     -12
    .set    OFS_PB,     -16
    .set    OFS_MAX,    -20
    .set    PATTERN_STEP, 0x9E3779B1
    .set    SIZE_1MB,   0x100000
    .set    SIZE_4KB,   0x1000
    .set    HIMEM_BASE_ADDR, 0x10000000

    .macro  PRT m
    pea     \m(%pc)
    .word   0xff09              | DOS _PRINT
    addq.l  #4,%sp
    .endm

    .macro  PRTV m, r           | メッセージ + 値(8桁16進)
    move.l  \r,-(%sp)
    PRT     \m
    move.l  (%sp)+,%d0
    bsr     phex
    .endm

    .macro  HIMEM cmd
    moveq   #\cmd,%d1
    move.l  #0xF8,%d0           | IOCS _HIMEM
    trap    #15
    .endm

    .macro  RESULT m            | Z=1 なら OK
    bne     7f
    PRT     s_ok
    bra     8f
7:  addq.l  #1,OFS_FAILS(%a6)
    PRT     s_ng
8:  PRT     \m
    .endm

_start:
    link    %a6,#-32
    clr.l   OFS_FAILS(%a6)

| ---- 1. VERSION
    HIMEM   5
    move.l  %d1,-(%sp)
    cmp.l   #0x30363054,%d0     | '060T'
    RESULT  m_version
    move.l  (%sp)+,%d0
    PRTV    m_ver_val,%d0

| ---- 2. GETSIZE / GETAREA
    HIMEM   3
    move.l  %d0,OFS_TOTAL0(%a6)
    move.l  %d1,OFS_MAX(%a6)
    PRTV    m_total,%d0
    move.l  OFS_MAX(%a6),%d0
    PRTV    m_max,%d0
    HIMEM   10
    move.l  %d1,-(%sp)
    PRTV    m_area_s,%d0
    move.l  (%sp)+,%d0
    PRTV    m_area_e,%d0

| ---- 3. MALLOC 1MB x2
    move.l  #SIZE_1MB,%d2
    HIMEM   1
    move.l  %a1,OFS_PA(%a6)
    tst.l   %d0
    RESULT  m_malloc_a_rc
    move.l  #SIZE_1MB,%d2
    HIMEM   1
    move.l  %a1,OFS_PB(%a6)
    tst.l   %d0
    RESULT  m_malloc_b_rc
    PRTV    m_ptr_a,OFS_PA(%a6)
    PRTV    m_ptr_b,OFS_PB(%a6)

    movea.l OFS_PA(%a6),%a1
    cmpa.l  #HIMEM_BASE_ADDR,%a1
    scc     %d0
    cmp.b   #-1,%d0
    RESULT  m_in_himem

    movea.l OFS_PA(%a6),%a1
    move.l  #SIZE_1MB,%d2
    move.l  #0x11111111,%d3
    bsr     fill
    movea.l OFS_PB(%a6),%a1
    move.l  #SIZE_1MB,%d2
    move.l  #0x77777777,%d3
    bsr     fill
    movea.l OFS_PA(%a6),%a1
    move.l  #SIZE_1MB,%d2
    move.l  #0x11111111,%d3
    bsr     verify
    tst.l   %d0
    RESULT  m_verify_a
    movea.l OFS_PB(%a6),%a1
    move.l  #SIZE_1MB,%d2
    move.l  #0x77777777,%d3
    bsr     verify
    tst.l   %d0
    RESULT  m_verify_b

| ---- 4. FREE
    move.l  OFS_PA(%a6),%d2
    HIMEM   2
    tst.l   %d0
    RESULT  m_free_a
    move.l  OFS_PB(%a6),%d2
    HIMEM   2
    tst.l   %d0
    RESULT  m_free_b
    HIMEM   3
    cmp.l   OFS_TOTAL0(%a6),%d0
    RESULT  m_total_back

| ---- 5. 最大ブロック（先頭/末尾 4KB）
    move.l  OFS_MAX(%a6),%d2
    HIMEM   1
    move.l  %a1,OFS_PA(%a6)
    tst.l   %d0
    RESULT  m_malloc_max_rc
    PRTV    m_ptr_a,OFS_PA(%a6)
    movea.l OFS_PA(%a6),%a1
    move.l  #SIZE_4KB,%d2
    move.l  #0x22222222,%d3
    bsr     fill
    movea.l OFS_PA(%a6),%a1
    adda.l  OFS_MAX(%a6),%a1
    suba.l  #SIZE_4KB,%a1
    move.l  #SIZE_4KB,%d2
    move.l  #0x33333333,%d3
    bsr     fill
    movea.l OFS_PA(%a6),%a1
    move.l  #SIZE_4KB,%d2
    move.l  #0x22222222,%d3
    bsr     verify
    tst.l   %d0
    RESULT  m_verify_head
    movea.l OFS_PA(%a6),%a1
    adda.l  OFS_MAX(%a6),%a1
    suba.l  #SIZE_4KB,%a1
    move.l  #SIZE_4KB,%d2
    move.l  #0x33333333,%d3
    bsr     verify
    tst.l   %d0
    RESULT  m_verify_tail
    move.l  OFS_PA(%a6),%d2
    HIMEM   2
    tst.l   %d0
    RESULT  m_free_max

| ---- 6. CALLOC（直前まで使っていた領域に残るパターンが消えること）
    move.l  #256,%d2
    move.l  #4,%d3
    HIMEM   6
    move.l  %a1,OFS_PA(%a6)
    tst.l   %d0
    RESULT  m_calloc_rc
    movea.l OFS_PA(%a6),%a0
    moveq   #0,%d0
    move.w  #255,%d1
1:  or.l    (%a0)+,%d0
    dbra    %d1,1b
    tst.l   %d0
    RESULT  m_calloc_zero
    move.l  OFS_PA(%a6),%d2
    HIMEM   2

| ---- 6b. REALLOC 縮小（移動なしのはず）
    move.l  #0x10000,%d2
    HIMEM   1
    move.l  %a1,OFS_PA(%a6)
    movea.l OFS_PA(%a6),%a1
    move.l  #0x10000,%d2
    move.l  #0x55555555,%d3
    bsr     fill
    move.l  OFS_PA(%a6),%d2
    move.l  #0x8000,%d3
    HIMEM   7
    move.l  %d0,-(%sp)
    PRTV    m_ptr_old,OFS_PA(%a6)
    move.l  (%sp)+,%d0
    tst.l   %d0
    RESULT  m_shrink_rc
    PRTV    m_ptr_new,%a1
    cmpa.l  OFS_PA(%a6),%a1
    RESULT  m_shrink_same
    movea.l OFS_PA(%a6),%a1         | 返り値でなく旧ポインタで確認する
    move.l  #0x8000,%d2
    move.l  #0x55555555,%d3
    bsr     verify
    tst.l   %d0
    RESULT  m_shrink_keep
    move.l  OFS_PA(%a6),%d2
    HIMEM   2
    PRTV    m_free_rc,%d0

| ---- 6c. HIMEM_RESIZE（移動しない）
    move.l  #0x10000,%d2
    HIMEM   1
    move.l  %a1,OFS_PA(%a6)
    move.l  OFS_PA(%a6),%d2
    move.l  #0x20000,%d3
    HIMEM   4
    tst.l   %d0
    RESULT  m_resize_rc
    move.l  OFS_PA(%a6),%d2
    HIMEM   2
    PRTV    m_free_rc,%d0

| ---- 7. REALLOC 64KB -> 256KB
    move.l  #0x10000,%d2
    HIMEM   1
    move.l  %a1,OFS_PA(%a6)
    movea.l OFS_PA(%a6),%a1
    move.l  #0x10000,%d2
    move.l  #0x44444444,%d3
    bsr     fill
    move.l  OFS_PA(%a6),%d2
    move.l  #0x40000,%d3
    PRTV    m_ptr_old,OFS_PA(%a6)
    HIMEM   7
    move.l  %a1,OFS_PB(%a6)
    tst.l   %d0
    RESULT  m_realloc_rc
    PRTV    m_ptr_new,OFS_PB(%a6)
    movea.l OFS_PB(%a6),%a1
    cmpa.l  OFS_PA(%a6),%a1
    RESULT  m_realloc_same
    movea.l OFS_PA(%a6),%a1         | 返り値でなく旧ポインタで確認する
    move.l  #0x10000,%d2
    move.l  #0x44444444,%d3
    bsr     verify
    move.l  %d0,-(%sp)
    PRTV    m_mismatch,%d0
    move.l  (%sp)+,%d0
    tst.l   %d0
    RESULT  m_realloc_keep
    move.l  OFS_PA(%a6),%d2
    HIMEM   2
    PRTV    m_free_rc,%d0
    HIMEM   3
    move.l  %d0,-(%sp)
    PRTV    m_total,%d0
    move.l  (%sp)+,%d0
    cmp.l   OFS_TOTAL0(%a6),%d0
    RESULT  m_total_back2

| ---- 結果
    PRTV    m_fails,OFS_FAILS(%a6)
    unlk    %a6
    .word   0xff00              | DOS _EXIT

| a1=先頭 d2=バイト数(4の倍数) d3=シード
fill:
    movem.l %d1/%d4/%a0,-(%sp)
    movea.l %a1,%a0
    move.l  %d2,%d1
    lsr.l   #2,%d1
    move.l  %d3,%d4
1:  move.l  %d4,(%a0)+
    add.l   #PATTERN_STEP,%d4
    subq.l  #1,%d1
    bne     1b
    movem.l (%sp)+,%d1/%d4/%a0
    rts

| a1=先頭 d2=バイト数 d3=シード -> d0=不一致数
verify:
    movem.l %d1/%d4/%a0,-(%sp)
    movea.l %a1,%a0
    move.l  %d2,%d1
    lsr.l   #2,%d1
    move.l  %d3,%d4
    moveq   #0,%d0
1:  cmp.l   (%a0)+,%d4
    beq     2f
    addq.l  #1,%d0
2:  add.l   #PATTERN_STEP,%d4
    subq.l  #1,%d1
    bne     1b
    movem.l (%sp)+,%d1/%d4/%a0
    rts

| d0 を8桁16進＋CRLFで表示（全レジスタ保存）
phex:
    movem.l %d0-%d2/%a0,-(%sp)
    lea     -12(%sp),%sp
    lea     (%sp),%a0
    moveq   #7,%d2
1:  rol.l   #4,%d0
    move.l  %d0,%d1
    and.l   #15,%d1
    add.b   #'0',%d1
    cmp.b   #'9',%d1
    ble     2f
    add.b   #7,%d1
2:  move.b  %d1,(%a0)+
    dbra    %d2,1b
    move.b  #13,(%a0)+
    move.b  #10,(%a0)+
    clr.b   (%a0)
    pea     (%sp)
    .word   0xff09              | DOS _PRINT
    addq.l  #4,%sp
    lea     12(%sp),%sp
    movem.l (%sp)+,%d0-%d2/%a0
    rts

s_ok:           .asciz  "OK: "
s_ng:           .asciz  "NG: "
m_version:      .asciz  "VERSION == '060T'\r\n"
m_ver_val:      .asciz  "  version = $"
m_total:        .asciz  "  GETSIZE total = $"
m_max:          .asciz  "  GETSIZE max   = $"
m_area_s:       .asciz  "  GETAREA start = $"
m_area_e:       .asciz  "  GETAREA end   = $"
m_malloc_a_rc:  .asciz  "MALLOC 1MB (A) rc==0\r\n"
m_malloc_b_rc:  .asciz  "MALLOC 1MB (B) rc==0\r\n"
m_ptr_a:        .asciz  "  block A = $"
m_ptr_b:        .asciz  "  block B = $"
m_in_himem:     .asciz  "block A >= $10000000\r\n"
m_verify_a:     .asciz  "write/read 1MB (A)\r\n"
m_verify_b:     .asciz  "write/read 1MB (B) (no overlap)\r\n"
m_free_a:       .asciz  "FREE A rc==0\r\n"
m_free_b:       .asciz  "FREE B rc==0\r\n"
m_total_back:   .asciz  "GETSIZE total restored after FREE\r\n"
m_malloc_max_rc: .asciz "MALLOC max block rc==0\r\n"
m_verify_head:  .asciz  "write/read max block head 4KB\r\n"
m_verify_tail:  .asciz  "write/read max block tail 4KB\r\n"
m_free_max:     .asciz  "FREE max block rc==0\r\n"
m_calloc_rc:    .asciz  "CALLOC 256x4 rc==0\r\n"
m_calloc_zero:  .asciz  "CALLOC block is zero-filled\r\n"
m_realloc_rc:   .asciz  "REALLOC 64KB->256KB rc==0\r\n"
m_realloc_keep: .asciz  "REALLOC keeps contents\r\n"
m_total_back2:  .asciz  "GETSIZE total restored at end\r\n"
m_ptr_old:      .asciz  "  realloc old = $"
m_ptr_new:      .asciz  "  realloc new = $"
m_mismatch:     .asciz  "  mismatches  = $"
m_free_rc:      .asciz  "  FREE rc     = $"
m_shrink_rc:    .asciz  "REALLOC shrink rc==0\r\n"
m_shrink_same:  .asciz  "REALLOC shrink keeps address\r\n"
m_shrink_keep:  .asciz  "REALLOC shrink keeps contents\r\n"
m_resize_rc:    .asciz  "RESIZE 64KB->128KB rc==0\r\n"
m_realloc_same: .asciz  "REALLOC grow keeps address (a1 == old)\r\n"
m_fails:        .asciz  "NG count = $"
    .even
