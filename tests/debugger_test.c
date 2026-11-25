/*
 * MCPデバッガコマンド（docs/ix68k-internals/design/mcp_design.md §3・§5）の単体テスト。
 * x68k_bus + libmc68000を直接リンクしSDL非依存で回す。
 *
 * 1. peek無副作用テスト（最重要）: FDCを結果フェーズにしてからx68k_bus_peek8()を
 *    繰り返し呼んでもres_idxが進まないこと。x68k_bus_read8()は進むこと（対比）。
 * 2. peek値一致テスト: RAM/GVRAM/TVRAM/SRAM/ROM各領域でpeek8とread8の値が一致。
 * 3. 逆アセンブル: 既知のバイト列を書いてmc68k_disassemble()の文字列と戻り長を検証
 *    （peekフック配線がmc68000コアまで通っていることの確認。ニーモニック整形の
 *    網羅的な正しさはmc68000側のテストで担保済みのためここでは検証しない）。
 * 4. 条件式パーサ: レジスタ比較・メモリ比較・パスカウント・AND結合・
 *    不正入力のエラー。
 * 5. MCPデバッガ実行制御: dbg_exec()が実際に通常／debug実行経路を選ぶこと、
 *    および例外・トレース・HALTEDのstopinfo契約を実CPU実行で検証する。
 */
#include "app_internal.h"
#include "bus/bus_internal.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "mcp/dbg_cond.h"
#include "mcp/debugger.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* debugger.cが参照するアプリ層の最小スタブ。 */
x68k_app_t s_app;

void x68k_cycle_catchup(x68k_bus_t *bus, int ran) {
    (void)bus;
    (void)ran;
}

static uint8_t peek8_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek8((const x68k_bus_t *)ctx, addr);
}
static uint16_t peek16_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek16((const x68k_bus_t *)ctx, addr);
}
static uint32_t peek32_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek32((const x68k_bus_t *)ctx, addr);
}

static void write_long_be(x68k_bus_t *b, uint32_t addr, uint32_t value) {
    x68k_bus_write8(b, addr, (uint8_t)(value >> 24));
    x68k_bus_write8(b, addr + 1, (uint8_t)(value >> 16));
    x68k_bus_write8(b, addr + 2, (uint8_t)(value >> 8));
    x68k_bus_write8(b, addr + 3, (uint8_t)value);
}

static void write_word_be(x68k_bus_t *b, uint32_t addr, uint16_t value) {
    x68k_bus_write8(b, addr, (uint8_t)(value >> 8));
    x68k_bus_write8(b, addr + 1, (uint8_t)value);
}

/* 1. peek無副作用テスト。 */
static void test_peek_no_side_effect(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    b->fdc.phase = FDC_RESULT;
    b->fdc.res[0] = 0x11;
    b->fdc.res[1] = 0x22;
    b->fdc.res[2] = 0x33;
    b->fdc.res_len = 3;
    b->fdc.res_idx = 0;

    for (int i = 0; i < 100; i++) {
        /* FDC結果レジスタはI/O領域としてpeek8の対象外（固定0x00を返す設計）。
         * ここで検証したいのはres_idx/phaseに一切触れないことそのもの。 */
        uint8_t v = x68k_bus_peek8(b, 0xE94003u);
        assert(v == 0x00);
        assert(b->fdc.res_idx == 0);
        assert(b->fdc.phase == FDC_RESULT);
    }

    /* 対比: read8(=x68k_bus_read8)はres_idxを進める既存の副作用を持つ。 */
    uint8_t v = x68k_bus_read8(b, 0xE94003u);
    assert(v == 0x11);
    assert(b->fdc.res_idx == 1);
    assert(b->fdc.phase == FDC_RESULT);

    printf("test_peek_no_side_effect: OK\n");
}

/* 2. peek値一致テスト。 */
static void test_peek_matches_read_for_backed_regions(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* RAM */
    x68k_bus_write8(b, 0x001000u, 0xABu);
    assert(x68k_bus_peek8(b, 0x001000u) == x68k_bus_read8(b, 0x001000u));

    /* GVRAM */
    x68k_bus_write8(b, GVRAM_BASE + 0x100u, 0xCDu);
    assert(x68k_bus_peek8(b, GVRAM_BASE + 0x100u) == x68k_bus_read8(b, GVRAM_BASE + 0x100u));

    /* TVRAM */
    x68k_bus_write8(b, TVRAM_BASE + 0x200u, 0xEFu);
    assert(x68k_bus_peek8(b, TVRAM_BASE + 0x200u) == x68k_bus_read8(b, TVRAM_BASE + 0x200u));

    /* SRAM */
    x68k_bus_write8(b, SRAM_BASE + 0x10u, 0x5Au);
    assert(x68k_bus_peek8(b, SRAM_BASE + 0x10u) == x68k_bus_read8(b, SRAM_BASE + 0x10u));

    /* ROM（IPLROM側。iocs_romへ直接書いてbacking storeを模擬） */
    b->iocs_rom[0x0D0000u] = 0x77u;
    assert(x68k_bus_peek8(b, 0xFF0000u) == x68k_bus_read8(b, 0xFF0000u));
    assert(x68k_bus_peek8(b, 0xFF0000u) == 0x77u);

    /* CRTC（MFPミラーでない通常レジスタ）: read8/peek8とも副作用なし。 */
    x68k_bus_write8(b, 0xE80000u, 0x12u);
    assert(x68k_bus_peek8(b, 0xE80000u) == x68k_bus_read8(b, 0xE80000u));

    printf("test_peek_matches_read_for_backed_regions: OK\n");
}

/* アドレスデコードを領域ハンドラへ分けても、読出しで消費されるレジスタと
 * 非侵襲peekの境界が変わらないことを確認する。 */
static void test_decode_read_clear_boundaries(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);

    /* CRTC action port: peekは保留状態を消費せず、readは最初の1回だけbit1を返す。 */
    b->crtc.rapid_clear_pending = 1;
    b->crtc_regs[0x481] = 0x02u;
    assert(x68k_bus_peek8(b, 0xE80481u) == 0x02u);
    assert(b->crtc.rapid_clear_pending == 1);
    assert(x68k_bus_read8(b, 0xE80481u) == 0x02u);
    assert(b->crtc.rapid_clear_pending == 0);
    assert(x68k_bus_read8(b, 0xE80481u) == 0x00u);

    /* PPI port C はADPCM側レジスタを経由する。 */
    x68k_bus_write8(b, 0xE9A005u, 0x55u);
    assert(x68k_bus_read8(b, 0xE9A005u) == 0x55u);

    printf("test_decode_read_clear_boundaries: OK\n");
}

/* 3. 逆アセンブル（peekフック配線の確認）。 */
static void test_disassemble_via_peek_hooks(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create(&hooks);
    assert(cpu != NULL);
    mc68k_set_peek_hooks(cpu, peek8_thunk, peek16_thunk, peek32_thunk, b);

    /* NOP (0x4E71): 長さ2。 */
    x68k_bus_write8(b, 0x002000u, 0x4Eu);
    x68k_bus_write8(b, 0x002001u, 0x71u);
    char buf[128];
    int len = mc68k_disassemble(cpu, 0x002000u, buf, sizeof(buf));
    assert(len == 2);
    assert(strstr(buf, "NOP") != NULL);

    /* JMP $00FF1234.L (0x4EF9 00FF1234): 長さ6（mcp_design.md の例と同一バイト列）。 */
    static const uint8_t jmp_bytes[6] = {0x4E, 0xF9, 0x00, 0xFF, 0x12, 0x34};
    for (int i = 0; i < 6; i++)
        x68k_bus_write8(b, 0x002010u + (uint32_t)i, jmp_bytes[i]);
    len = mc68k_disassemble(cpu, 0x002010u, buf, sizeof(buf));
    assert(len == 6);
    assert(strstr(buf, "JMP") != NULL);

    /* ILLEGAL (0x4AFC): 未割り当てオペコードは長さ2で"ILLEGAL"。 */
    x68k_bus_write8(b, 0x002020u, 0x4Au);
    x68k_bus_write8(b, 0x002021u, 0xFCu);
    len = mc68k_disassemble(cpu, 0x002020u, buf, sizeof(buf));
    assert(len == 2);
    assert(strstr(buf, "ILLEGAL") != NULL);

    mc68k_destroy(cpu);
    printf("test_disassemble_via_peek_hooks: OK\n");
}

/* 5. PMMU 有効時の論理読み出し（mc68000側の設計項目）。
 *
 * peek / disassemble / 条件付きブレークポイントはいずれも論理アドレスで動く
 * べきであり、MC68030 の PMMU が有効なときはページテーブルを引いた先の物理
 * ページを読まなければならない。X68030 実機は MC68EC030（PMMU なし）で
 * logical = physical になるため、それだけでは論理読み出しになっているかを
 * 判定できない。ここでは MC68030 モデルで論理 $008000 を物理 $009000 へ
 * ずらしたページを作り、3 経路すべてが論理側を見ることを確認する。 */
static void test_logical_reads_follow_the_pmmu(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create_model(&hooks, MC68K_MODEL_68030);
    assert(cpu != NULL);
    mc68k_set_peek_hooks(cpu, peek8_thunk, peek16_thunk, peek32_thunk, b);

    /* 長形式ルート $4000 → A[0] はショートの B テーブル $5000。B テーブルは
     * $00000000-$0001FFFF を 4 KiB ページで恒等写像し、論理 $008000 だけを
     * 物理 $009000 へずらす。 */
    write_long_be(b, 0x4000u, 0x0000000Au); /* A[0]: DT=2（ショートテーブル）, U */
    write_long_be(b, 0x4004u, 0x00005000u);
    for (uint32_t page = 0; page < 0x20u; page++)
        write_long_be(b, 0x5000u + page * 4u, (page << 12) | 9u); /* DT=1, U */
    write_long_be(b, 0x5020u, 0x00009009u); /* 論理 $008xxx -> 物理 $009xxx */

    write_word_be(b, 0x8000u, 0x4E71u); /* 物理 $008000: NOP           */
    write_word_be(b, 0x9000u, 0x2200u); /* 物理 $009000: MOVE.L D0,D1  */

    /* ゲストコードで CRP と TC をロードする（内部ヘッダを使わない）。 */
    write_long_be(b, 0x0000u, 0x00003000u); /* SSP */
    write_long_be(b, 0x0004u, 0x00001000u); /* 初期 PC */
    write_long_be(b, 0x2000u, 0x80000003u); /* CRP 上位: 下限 LIMIT 0, DT=3 */
    write_long_be(b, 0x2004u, 0x00004000u); /* CRP 下位: ルートテーブル */
    write_long_be(b, 0x2008u, 0x82C0AA00u); /* TC: E, 4 KiB, IS=0, TIA=10, TIB=10 */
    write_word_be(b, 0x1000u, 0x207Cu); write_long_be(b, 0x1002u, 0x00002000u);
    write_word_be(b, 0x1006u, 0xF010u); write_word_be(b, 0x1008u, 0x4C00u); /* PMOVE (A0),CRP */
    write_word_be(b, 0x100Au, 0xF010u); write_word_be(b, 0x100Cu, 0x4800u); /* PMOVE (A0),SRP */
    write_word_be(b, 0x100Eu, 0x207Cu); write_long_be(b, 0x1010u, 0x00002008u);
    write_word_be(b, 0x1014u, 0xF010u); write_word_be(b, 0x1016u, 0x4000u); /* PMOVE (A0),TC */
    mc68k_reset(cpu);
    for (int i = 0; i < 5; i++)
        mc68k_execute(cpu, 1);
    mc68k_regs_t r;
    mc68k_get_regs(cpu, &r);
    assert(r.pc == 0x00001018u); /* 5 命令とも完走した（ベクタ 56 へ落ちていない） */
    assert(mc68k_get_exec_state(cpu) == CPU_NORMAL);

    /* TC.SRE が立っているので、スーパーバイザ FC の変換は SRP を根に使う。 */
    mc68k_mmu_regs_t m;
    mc68k_get_mmu_regs(cpu, &m);
    assert(m.tc == 0x82C0AA00u);
    assert(m.crp == 0x8000000300004000ull && m.srp == m.crp);

    /* PMMU変換後のMMIO到達: A[3] / B[$2af]で$EAFFEB（RTCテストバイト）を
     * 恒等変換し、ゲストのMOVE.Bがix68kバス上の実体へ届くことを確認する。 */
    write_long_be(b, 0x4000u + 3u * 8u, 0x00000002u);
    write_long_be(b, 0x4000u + 3u * 8u + 4u, 0x00006000u);
    write_long_be(b, 0x6000u + 0x2afu * 4u, 0x00EAF009u);
    write_word_be(b, 0x1018u, 0x13FCu); /* MOVE.B #$5A,$EAFFEB.L */
    write_word_be(b, 0x101Au, 0x005Au);
    write_long_be(b, 0x101Cu, 0x00EAFFEBu);
    mc68k_execute(cpu, 1);
    assert(x68k_bus_read8(b, 0xEAFFEBu) == 0x5Au);

    /* (a) mcp_cmd_peek_mem() の読み出し経路。論理 $008000 は物理 $009000 を返す。 */
    uint8_t buf[4] = {0};
    assert(mc68k_debug_read_bytes(cpu, 0x008000u, buf, 2) == 0);
    assert(buf[0] == 0x22u && buf[1] == 0x00u);
    /* 物理 API は変換しないので、同じアドレスでも $008000 の中身を返す。 */
    assert(mc68k_debug_read_physical_bytes(cpu, 0x008000u, buf, 2) == 0);
    assert(buf[0] == 0x4Eu && buf[1] == 0x71u);

    /* (b) mcp_cmd_disasm() の逆アセンブル経路。 */
    char text[128];
    int len = mc68k_disassemble(cpu, 0x008000u, text, sizeof(text));
    assert(len == 2);
    assert(strstr(text, "MOVE.L D0,D1") != NULL);

    /* (c) dbg_cond_eval() のメモリ項。 */
    {
        dbg_cond_t cond;
        assert(dbg_cond_parse("[008000].w==2200", &cond) == 0);
        assert(dbg_cond_eval(&cond, cpu) != 0);
        assert(dbg_cond_parse("[008000].w==4e71", &cond) == 0);
        assert(dbg_cond_eval(&cond, cpu) == 0);
    }

    /* 未マップの論理アドレスは失敗を返すだけで、CPU を止めない。 */
    assert(mc68k_debug_read_bytes(cpu, 0x00400000u, buf, 2) == -1);
    assert(mc68k_get_exec_state(cpu) == CPU_NORMAL);

    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
    printf("test_logical_reads_follow_the_pmmu: OK\n");
}

/* 4. 条件式パーサ・評価器。 */
static void test_cond_parser(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create(&hooks);
    assert(cpu != NULL);
    mc68k_set_peek_hooks(cpu, peek8_thunk, peek16_thunk, peek32_thunk, b);

    /* レジスタ比較。 */
    {
        dbg_cond_t cond;
        assert(dbg_cond_parse("D0==12345678", &cond) == 0);
        assert(cond.term_count == 1);
        mc68k_set_dreg(cpu, 0, 0x12345678u);
        assert(dbg_cond_eval(&cond, cpu) != 0);
        mc68k_set_dreg(cpu, 0, 0x11111111u);
        assert(dbg_cond_eval(&cond, cpu) == 0);
    }

    /* メモリ比較（非侵襲peek経由）。 */
    {
        dbg_cond_t cond;
        assert(dbg_cond_parse("[000807].b!=00", &cond) == 0);
        assert(cond.term_count == 1);
        x68k_bus_write8(b, 0x000807u, 0x00u);
        assert(dbg_cond_eval(&cond, cpu) == 0); /* 00 != 00 は偽 */
        x68k_bus_write8(b, 0x000807u, 0x5Au);
        assert(dbg_cond_eval(&cond, cpu) != 0); /* 5A != 00 は真 */
    }

    /* パスカウント: 最初のn回は無視し、n+1回目以降は常に真。 */
    {
        dbg_cond_t cond;
        assert(dbg_cond_parse("#3", &cond) == 0);
        assert(dbg_cond_eval(&cond, cpu) == 0);
        assert(dbg_cond_eval(&cond, cpu) == 0);
        assert(dbg_cond_eval(&cond, cpu) == 0);
        assert(dbg_cond_eval(&cond, cpu) != 0);
        assert(dbg_cond_eval(&cond, cpu) != 0);
    }

    /* AND結合（&&のみ）。 */
    {
        dbg_cond_t cond;
        assert(dbg_cond_parse("D0==5 && D1==10", &cond) == 0);
        assert(cond.term_count == 2);
        mc68k_set_dreg(cpu, 0, 5);
        mc68k_set_dreg(cpu, 1, 0x11u);
        assert(dbg_cond_eval(&cond, cpu) == 0); /* D1不一致 */
        mc68k_set_dreg(cpu, 1, 0x10u);
        assert(dbg_cond_eval(&cond, cpu) != 0); /* 両方一致 */
    }

    /* 不正入力はエラー（outは未変更の契約）。 */
    {
        dbg_cond_t cond;
        assert(dbg_cond_parse("D0##5", &cond) != 0);
        assert(dbg_cond_parse("XYZZY==1", &cond) != 0);
        assert(dbg_cond_parse("D0==", &cond) != 0);
        assert(dbg_cond_parse("D0==1 &&", &cond) != 0);
    }

    /* 無条件（NULL/空文字列）は常に真。 */
    {
        dbg_cond_t cond;
        assert(dbg_cond_parse("", &cond) == 0);
        assert(cond.term_count == 0);
        assert(dbg_cond_eval(&cond, cpu) != 0);
        assert(dbg_cond_parse(NULL, &cond) == 0);
        assert(dbg_cond_eval(&cond, cpu) != 0);
    }

    mc68k_destroy(cpu);
    printf("test_cond_parser: OK\n");
}

/* 5. ウォッチポイント: 値変化のエッジ検出のみ（mcp_design.md §3・§5）。
 * mc68k_watchpoint_check()自体をmc68k_debug_run()を介さず
 * 直接呼び、コアAPIの契約（変化時のみヒット・前回値は呼ぶたび更新）を確認する。 */
static void test_watchpoint_basic(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create(&hooks);
    assert(cpu != NULL);
    mc68k_set_peek_hooks(cpu, peek8_thunk, peek16_thunk, peek32_thunk, b);

    x68k_bus_write8(b, 0x000807u, 0x00u);
    int wp_id = mc68k_add_watchpoint(cpu, 0x000807u, 1);
    assert(wp_id >= 0);

    /* 値変化なし: ヒットしない。 */
    assert(mc68k_watchpoint_check(cpu) == -1);

    /* 値変化: ヒットしold/newが正しく取得できる。 */
    x68k_bus_write8(b, 0x000807u, 0x5Au);
    assert(mc68k_watchpoint_check(cpu) == wp_id);
    uint32_t old_v = 0xFFFFFFFFu, new_v = 0xFFFFFFFFu;
    int hit_id = mc68k_get_watchpoint_hit(cpu, &old_v, &new_v);
    assert(hit_id == wp_id);
    assert(old_v == 0x00u);
    assert(new_v == 0x5Au);

    /* 前回値が更新されるため、同じ値を再度checkしてもヒットしない
     * （エッジトリガの確認）。 */
    assert(mc68k_watchpoint_check(cpu) == -1);

    mc68k_destroy(cpu);
    printf("test_watchpoint_basic: OK\n");
}

/* ---- 6. 実行経路とstopinfo ---------------------- */

static void reset_debug_cpu(x68k_bus_t *b, mc68k_state_t *cpu, uint32_t sp) {
    write_long_be(b, 0x000000u, sp);
    write_long_be(b, 0x000004u, 0x001000u);
    mc68k_reset(cpu);
    dbg_continue();
    dbg_bp_clear();
    dbg_wp_clear();
    dbg_set_exc_mask(0);
    dbg_set_mode(DBG_MODE_AUTO);
    mc68k_set_pc_ring(cpu, 0);
    mc68k_set_reg_ring(cpu, 0);
}

/* 実装とは独立にレジスタ部を組み立て、stopinfo全体を完全一致で検証する。 */
static void assert_stopinfo_prefix(const char *prefix) {
    mc68k_regs_t r;
    mc68k_get_regs(s_app.cpu, &r);
    char actual[MCP_STOPINFO_MAX];
    char expected[MCP_STOPINFO_MAX];
    int actual_n = dbg_format_stopinfo(actual, sizeof(actual));
    int expected_n = snprintf(expected, sizeof(expected),
                              "%s"
                              "PC=%08X "
                              "D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X "
                              "A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X "
                              "SR=%04X USP=%08X SSP=%08X",
                              prefix, r.pc, r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6],
                              r.d[7], r.a[0], r.a[1], r.a[2], r.a[3], r.a[4], r.a[5], r.a[6], r.a[7],
                              r.sr, r.usp, r.ssp);
    assert(actual_n > 0 && expected_n > 0);
    assert((size_t)actual_n < sizeof(actual) && (size_t)expected_n < sizeof(expected));
    assert(strcmp(actual, expected) == 0);
}

static void test_debug_exec_and_stopinfo(void) {
    x68k_bus_t *b = x68k_bus_create();
    assert(b != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(b);
    mc68k_state_t *cpu = mc68k_create(&hooks);
    assert(cpu != NULL);
    mc68k_set_peek_hooks(cpu, peek8_thunk, peek16_thunk, peek32_thunk, b);
    s_app.cpu = cpu;
    s_app.bus = b;
    dbg_continue();

    uint64_t running_clock = x68k_bus_guest_mono_us(b);
    dbg_halt_whiteband();
    uint64_t frozen_clock = x68k_bus_guest_mono_us(b);
    assert(frozen_clock >= running_clock);
    assert(x68k_bus_guest_mono_us(b) == frozen_clock);
    dbg_continue();
    assert(x68k_bus_guest_mono_us(b) >= frozen_clock);

    /* BP登録時はdbg_exec()がdebug_runを使い、実際にBPで停止する。 */
    reset_debug_cpu(b, cpu, 0x008000u);
    write_word_be(b, 0x001000u, 0x4E71u); /* NOP */
    assert(dbg_bp_add(0x001002u, NULL) >= 0);
    assert(dbg_exec(cpu, 100) > 0);
    assert(dbg_after_chunk(b));
    assert(dbg_is_paused());
    assert_stopinfo_prefix("STATE=paused STOP=BREAKPOINT MODE=AUTO CAP=NONE EXEC=DEBUG WB=OFF BP=0 ");

    /* 同じBPが残っていてもMODE OFFは通常実行経路を強制し、停止しない。 */
    reset_debug_cpu(b, cpu, 0x008000u);
    write_word_be(b, 0x001000u, 0x4E71u);
    assert(dbg_bp_add(0x001002u, NULL) >= 0);
    dbg_set_mode(DBG_MODE_OFF);
    assert(dbg_exec(cpu, 4) > 0);
    assert(!dbg_after_chunk(b));
    assert(!dbg_is_paused());
    char running[MCP_STOPINFO_MAX];
    assert(dbg_format_stopinfo(running, sizeof(running)) > 0);
    assert(strcmp(running, "STATE=running MODE=OFF CAP=NONE EXEC=NORMAL WB=OFF") == 0);

    /* WP登録時も通常チャンクのdbg_exec()がdebug_run経路を選び、値変化を
     * 検出して停止する。DBG STEP経路だけでは、この自動実行経路を検証できない。 */
    reset_debug_cpu(b, cpu, 0x008000u);
    const uint32_t wp_addr = 0x002000u;
    write_word_be(b, 0x001000u, 0x4E71u); /* NOP */
    x68k_bus_write8(b, wp_addr, 0x00u);
    int wp_id = dbg_wp_add(wp_addr, 1);
    assert(wp_id >= 0);
    x68k_bus_write8(b, wp_addr, 0x5Au); /* OLD=00000000 -> NEW=0000005A */
    assert(dbg_exec(cpu, 100) > 0);
    assert(dbg_after_chunk(b));
    assert(dbg_is_paused());
    char wp_prefix[160];
    snprintf(wp_prefix, sizeof(wp_prefix),
             "STATE=paused STOP=WATCHPOINT MODE=AUTO CAP=NONE EXEC=DEBUG WB=OFF "
             "WP=%d WPADDR=%06X OLD=00000000 NEW=0000005A ",
             wp_id, wp_addr);
    assert_stopinfo_prefix(wp_prefix);

    /* WPを解除すれば、AUTOは再び通常実行経路を選ぶ。 */
    dbg_continue();
    dbg_wp_clear();
    assert(dbg_format_stopinfo(running, sizeof(running)) > 0);
    assert(strcmp(running, "STATE=running MODE=AUTO CAP=NONE EXEC=NORMAL WB=OFF") == 0);

    /* paused側でもCAPの全4状態を実効ring値から表す。停止理由にWHITEBANDを
     * 使うことで、CPU実行に依存せずCAPだけを変えた完全一致契約にする。 */
    reset_debug_cpu(b, cpu, 0x008000u);
    dbg_halt_whiteband();
    assert_stopinfo_prefix("STATE=paused STOP=WHITEBAND MODE=AUTO CAP=NONE EXEC=NORMAL WB=OFF ");
    mc68k_set_pc_ring(cpu, 1);
    assert_stopinfo_prefix("STATE=paused STOP=WHITEBAND MODE=AUTO CAP=PC EXEC=NORMAL WB=OFF ");
    mc68k_set_reg_ring(cpu, 1);
    assert_stopinfo_prefix("STATE=paused STOP=WHITEBAND MODE=AUTO CAP=PC,REG EXEC=NORMAL WB=OFF ");
    mc68k_set_pc_ring(cpu, 0);
    assert_stopinfo_prefix("STATE=paused STOP=WHITEBAND MODE=AUTO CAP=REG EXEC=NORMAL WB=OFF ");

    /* ILLEGAL(ベクタ4)は例外マスクでEXCEPTIONになり、ベクタ詳細も返す。 */
    reset_debug_cpu(b, cpu, 0x008000u);
    write_long_be(b, 4u * 4u, 0x001100u);
    write_word_be(b, 0x001000u, 0x4AFCu); /* ILLEGAL */
    write_word_be(b, 0x001100u, 0x4E71u);
    dbg_set_exc_mask(UINT64_C(1) << 4);
    assert(dbg_exec(cpu, 100) > 0);
    assert(dbg_after_chunk(b));
    assert_stopinfo_prefix("STATE=paused STOP=EXCEPTION MODE=AUTO CAP=NONE EXEC=DEBUG WB=OFF VEC=4 ");

    /* MOVE #$A700,SRでゲストのtrace bitを立て、ベクタ9停止を実発生させる。 */
    reset_debug_cpu(b, cpu, 0x008000u);
    write_long_be(b, 9u * 4u, 0x001100u);
    write_word_be(b, 0x001000u, 0x46FCu); /* MOVE.W #imm,SR */
    write_word_be(b, 0x001002u, 0xA700u); /* T1 + S + IPL=7 */
    write_word_be(b, 0x001100u, 0x4E71u);
    dbg_set_exc_mask(UINT64_C(1) << 9);
    assert(dbg_exec(cpu, 100) > 0);
    assert(dbg_after_chunk(b));
    assert_stopinfo_prefix("STATE=paused STOP=TRACE MODE=AUTO CAP=NONE EXEC=DEBUG WB=OFF VEC=9 ");

    /* oddなSPでILLEGALをdispatchすると例外フレームのpushがアドレスエラーとなり、
     * コアはHALTEDへ遷移する。HALTEDは例外マスクに関わらず停止する。 */
    reset_debug_cpu(b, cpu, 0x008001u);
    write_word_be(b, 0x001000u, 0x4AFCu);
    dbg_set_mode(DBG_MODE_ON);
    assert(dbg_exec(cpu, 100) > 0);
    assert(dbg_after_chunk(b));
    assert_stopinfo_prefix("STATE=paused STOP=HALTED MODE=ON CAP=NONE EXEC=DEBUG WB=OFF ");

    s_app.cpu = NULL;
    s_app.bus = NULL;
    mc68k_destroy(cpu);
    x68k_bus_destroy(b);
    printf("test_debug_exec_and_stopinfo: OK\n");
}

int main(void) {
    test_peek_no_side_effect();
    test_peek_matches_read_for_backed_regions();
    test_decode_read_clear_boundaries();
    test_disassemble_via_peek_hooks();
    test_cond_parser();
    test_watchpoint_basic();
    test_debug_exec_and_stopinfo();
    test_logical_reads_follow_the_pmmu();
    printf("debugger_test: all tests passed\n");
    return 0;
}
