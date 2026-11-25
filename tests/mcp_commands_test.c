/*
 * MCPワイヤコマンド（`mcp_commands_handle()`）の単体テスト。
 *
 * ワイヤ層はこれまでテストから一度も通っていなかった（`tests/debugger_test.c`は
 * 条件式パーサとpeekまで）。コマンド文字列を渡して応答文字列を検証する形なら、
 * SDLもソケットも要らずに素早く回せる。48本のフラットコマンドをグループ化する
 * 再設計の回帰網としても使う。
 *
 * `mcp_reply`/`mcp_reply_bin`は`internal.h`宣言・`server.c`定義というリンク時の
 * 継ぎ目なので、`server.c`をリンクせずここで定義すれば本番コードを変えずに応答を
 * 捕まえられる（tagの付与・フレーミングはserver.cの責務でcommands.cは関知しない
 * ので、ここでもtagは一切扱わない）。アプリ層（`s_app`と`app_*`、
 * `x68k_cycle_catchup`）と、レンダラ一式を要する`mcp_screenshot_make_ppm()`も
 * ここでスタブする。
 */
#include "app_internal.h"
#include "app/checkpoint.h"
#include "bus/x68k_bus.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include "mcp/commands.h"
#include "mcp/debugger.h"
#include "mcp/internal.h"
#include "mcp/perf.h"
#include "mcp/text_event.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- アプリ層スタブ ---------------------------------------------------- */

x68k_app_t s_app = {.native_w = 512, .native_h = 512};

static int s_reset_calls, s_fd0_begin_calls, s_fd0_cancel_calls;

void app_reset_machine(x68k_bus_t *bus) {
    (void)bus;
    s_reset_calls++;
}
void app_force_fd0_boot_begin(x68k_bus_t *bus) {
    (void)bus;
    s_fd0_begin_calls++;
}
void app_force_fd0_boot_cancel(x68k_bus_t *bus) {
    (void)bus;
    s_fd0_cancel_calls++;
}
void x68k_cycle_catchup(x68k_bus_t *bus, int ran) {
    (void)bus;
    (void)ran;
}
void iocs_key_wait_input_queued(void) {
    s_app.key_input_waiting = 0;
}
void iocs_key_wait_poll(x68k_bus_t *bus) {
    (void)bus;
}

/* SCREENSHOTはレンダラ一式を要するので、長さだけ意味のあるダミーを返す。 */
uint8_t *mcp_screenshot_make_ppm(x68k_bus_t *bus, int mask, int *out_len) {
    (void)bus;
    (void)mask;
    uint8_t *buf = malloc(8);
    if (!buf)
        return NULL;
    memcpy(buf, "P6\n1 1\n", 7);
    buf[7] = '\n';
    *out_len = 8;
    return buf;
}

/* ---- 応答キャプチャ（server.c の代わり） ------------------------------- */

static char s_reply[64 * 1024];
static size_t s_reply_len;
static uint8_t s_bin_data[64 * 1024];
static size_t s_bin_len;
static int s_bin_called;

int mcp_reply(const char *text, size_t len) {
    if (s_reply_len + len < sizeof(s_reply)) {
        memcpy(s_reply + s_reply_len, text, len);
        s_reply_len += len;
        s_reply[s_reply_len] = '\0';
    }
    return 1;
}

int mcp_reply_bin(const uint8_t *data, size_t len) {
    size_t copy_len = len < sizeof(s_bin_data) ? len : sizeof(s_bin_data);
    memcpy(s_bin_data, data, copy_len);
    s_bin_len = len;
    s_bin_called = 1;
    return 1;
}

/* ---- テスト土台 -------------------------------------------------------- */

static uint8_t peek8_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek8((const x68k_bus_t *)ctx, addr);
}
static uint16_t peek16_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek16((const x68k_bus_t *)ctx, addr);
}
static uint32_t peek32_thunk(void *ctx, uint32_t addr) {
    return x68k_bus_peek32((const x68k_bus_t *)ctx, addr);
}

static x68k_bus_t *s_bus;
static mcp_context_t s_ctx;
static uint8_t s_output[16 * 1024];

/* コマンドを1本流し、応答文字列を返す（認識されたことも確認する）。 */
static const char *run_body(const char *line, const uint8_t *body, size_t body_len) {
    s_reply_len = 0;
    s_reply[0] = '\0';
    s_bin_len = 0;
    s_bin_called = 0;
    s_ctx.request_body = body;
    s_ctx.request_body_len = body_len;
    int handled = mcp_commands_handle(s_bus, &s_ctx, line);
    assert(handled && "コマンドが認識されなかった");
    s_ctx.request_body = NULL;
    s_ctx.request_body_len = 0;
    return s_reply;
}

static const char *run(const char *line) {
    return run_body(line, NULL, 0);
}

/* mcp_reply_bin で送られた本体をそのままコピーして長さを返す
 * （旧・base64デコードの置き換え。呼ばれていなければ落とす）。 */
static int decode_reply(uint8_t *out, int outmax) {
    assert(s_bin_called && "mcp_reply_bin が呼ばれなかった");
    int n = (int)s_bin_len;
    if (n > outmax)
        n = outmax;
    memcpy(out, s_bin_data, (size_t)n);
    return n;
}

/*
 * stopinfo応答（"OK <本文>\n"）を末尾まで完全一致で検証する。
 * prefixは "STATE=paused STOP=... WB=... <追加フィールド>" 等、レジスタ部
 * より前の全体。レジスタ部はdbg_format_stopinfo()を呼ばず、mc68k_get_regs()
 * から独立に組み立てる（実装の書式を変えたらテストが落ちる契約テスト）。
 */
static void assert_stopinfo(const char *prefix) {
    assert(strncmp(s_reply, "ERR INTERNAL ", 13) != 0);
    mc68k_regs_t r;
    mc68k_get_regs(s_app.cpu, &r);
    char expected[MCP_STOPINFO_MAX + 16];
    int n = snprintf(expected, sizeof(expected),
                     "OK %s"
                     "PC=%08X "
                     "D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X "
                     "A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X "
                     "SR=%04X USP=%08X SSP=%08X\n",
                     prefix, r.pc, r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6],
                     r.d[7], r.a[0], r.a[1], r.a[2], r.a[3], r.a[4], r.a[5], r.a[6], r.a[7],
                     r.sr, r.usp, r.ssp);
    assert(n > 0 && (size_t)n < sizeof(expected));
    /* stopinfo本体（"OK "と末尾"\n"を除く）がMCP_STOPINFO_MAXに収まることも
     * ここで担保する（サイズが十分であることの回帰）。 */
    assert((size_t)(n - 4) < MCP_STOPINFO_MAX);
    assert(strcmp(s_reply, expected) == 0);
}

static void setup(void) {
    s_bus = x68k_bus_create();
    assert(s_bus != NULL);
    mc68k_bus_t hooks = x68k_bus_get_hooks(s_bus);
    s_app.cpu = mc68k_create(&hooks);
    assert(s_app.cpu != NULL);
    s_app.bus = s_bus;
    x68k_bus_set_cpu(s_bus, s_app.cpu);
    mc68k_set_peek_hooks(s_app.cpu, peek8_thunk, peek16_thunk, peek32_thunk, s_bus);
    /* RING系ダンプ（dbg_dump_*_ring）はmc68k_set_log_handler経由でテキストを
     * キャプチャする。未登録だとcpu_log()が既定のstderr出力に落ち、
     * mcp_reply_binへ届かない。 */
    dbg_init_log_handler();
    s_ctx.wait_kind = MCP_WAIT_NONE;
    s_ctx.output = s_output;
    s_ctx.output_capacity = (int)sizeof(s_output);
    s_ctx.request_body = NULL;
    s_ctx.request_body_len = 0;
    checkpoint_init(NULL);
}

/* ---- 1. ディスパッチ --------------------------------------------------- */

static void test_dispatch(void) {
    /* 未知のコマンドは「認識しなかった」(0) を返し、応答も出さない
     * （ERR unknown を返すのは server.c 側の責務）。 */
    s_reply_len = 0;
    s_reply[0] = '\0';
    assert(mcp_commands_handle(s_bus, &s_ctx, "NO_SUCH_GROUP FOO") == 0);
    assert(s_reply_len == 0);

    /* 既知グループの未知アクションは、グループ側で ERR UNKNOWN を返す
     * （server.c へ落とさない＝「そんなグループは無い」と区別できる）。 */
    assert(strncmp(run("DBG NOPE"), "ERR UNKNOWN ", 12) == 0);

    /* HELLO はプロトコル版を返す。クライアントはこれで版ずれを検出する。 */
    run("HELLO");
    assert(strncmp(s_reply, "OK PROTO=5 BUILD=", 17) == 0);

    /* グループ＋アクションのテーブル駆動ディスパッチ。 */
    assert(strcmp(run("DIAG IOCS ON"), "OK\n") == 0);
    assert(s_app.trace_iocs == 1);
    run("DIAG IOCS OFF");
    assert(s_app.trace_iocs == 0);

    run("DIAG PCRING ON");
    assert(mc68k_get_pc_ring(s_app.cpu) == 1);
    run("DIAG PCRING OFF");
    assert(mc68k_get_pc_ring(s_app.cpu) == 0);

    /* フォールトリング。DIAG FAULTRING でON/OFFし、
     * RING FAULTRING でダンプを取得できる（記録0件でもヘッダ/フッタ行は返る）。 */
    run("DIAG FAULTRING ON");
    assert(mc68k_get_fault_ring(s_app.cpu) == 1);
    run("RING FAULTRING");
    uint8_t fault_text[256];
    int fault_len = decode_reply(fault_text, (int)sizeof(fault_text) - 1);
    fault_text[fault_len] = '\0';
    assert(strstr((char *)fault_text, "fault trace") != NULL);
    run("DIAG FAULTRING OFF");
    assert(mc68k_get_fault_ring(s_app.cpu) == 0);

    /* CALLRING は常時記録なので DIAG 不要。旧名 CALL は受け付けない。 */
    run("RING CALLRING");
    uint8_t call_text[256];
    int call_len = decode_reply(call_text, (int)sizeof(call_text) - 1);
    call_text[call_len] = '\0';
    assert(strstr((char *)call_text, "call trace") != NULL);
    assert(strncmp(run("RING CALL"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("RING PC"), "ERR BADARG ", 11) == 0);

    run("WHITEBAND ON");
    assert(s_app.whiteband_halt_on == 1);
    /* 状態表示に白帯HALTのarmed状態が出る。停止しないのが
     * 「ONにし忘れ」なのか「白帯が出ていない」のかを切り分けるため。 */
    run("DBG STATUS");
    assert(strstr(s_reply, "WB=ON") != NULL);
    run("WHITEBAND OFF");
    assert(s_app.whiteband_halt_on == 0);
    run("DBG STATUS");
    assert(strstr(s_reply, "WB=OFF") != NULL);

    /* 白帯検出はデバッガの停止として表現される。statusは
     * pausedを返し、whiteband(action="continue")=CONTINUE でも
     * debug_control(action="continue")=DBG_CONT でも再開できる。 */
    dbg_halt_whiteband();
    assert(dbg_is_paused());
    run("DBG STATUS");
    assert_stopinfo("STATE=paused STOP=WHITEBAND MODE=AUTO CAP=PC EXEC=NORMAL WB=OFF ");
    run("WHITEBAND CONT");
    assert(!dbg_is_paused());
    dbg_halt_whiteband();
    run("DBG CONT");
    assert(!dbg_is_paused());

    int resets = s_reset_calls;
    run("RESET");
    assert(s_reset_calls == resets + 1 && s_fd0_cancel_calls > 0);
    int begins = s_fd0_begin_calls;
    run("RESET FD0");
    assert(s_fd0_begin_calls == begins + 1);

    printf("test_dispatch: OK\n");
}

/* ---- 2. メモリ読み書き ------------------------------------------------- */

static void test_memory(void) {
    for (int i = 0; i < 16; i++)
        x68k_bus_write_byte(s_bus, 0x001000u + (uint32_t)i, (uint8_t)(0xA0 + i));

    uint8_t buf[512];
    run("MEM PEEK 1000 10");
    assert(decode_reply(buf, sizeof(buf)) == 16);
    assert(buf[0] == 0xA0 && buf[15] == 0xAF);

    run("MEM PEEK 1000 10 SUPER");
    assert(decode_reply(buf, sizeof(buf)) == 16);
    assert(buf[0] == 0xA0 && buf[15] == 0xAF);
    assert(strncmp(run("MEM PEEK 1000 10 BOGUS"), "ERR BADARG", 10) == 0);

    run("MEM READ 1000 10");
    assert(decode_reply(buf, sizeof(buf)) == 16);
    assert(buf[0] == 0xA0);

    /* 長さは256バイトへクランプされる。 */
    run("MEM READ 1000 400");
    assert(decode_reply(buf, sizeof(buf)) == 256);

    /* MEM WRITE は本文（フレームのボディ）に生バイト列を渡す（base64廃止）。 */
    static const uint8_t abc[] = {'A', 'B', 'C'};
    assert(strcmp(run_body("MEM WRITE 2000", abc, sizeof(abc)), "OK\n") == 0);
    assert(x68k_bus_read8(s_bus, 0x002000u) == 'A');
    assert(x68k_bus_read8(s_bus, 0x002002u) == 'C');

    assert(strncmp(run("MEM WRITE"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("MEM WRITE 2000"), "ERR BADARG ", 11) == 0); /* 本文なし */
    {
        static uint8_t big[257];
        assert(strncmp(run_body("MEM WRITE 2000", big, sizeof(big)), "ERR BADARG ", 11) == 0);
    }

    printf("test_memory: OK\n");
}

/* ---- 3. 大きな応答（回帰、base64廃止後） -------------- */

static void test_large_reply(void) {
    /* 8KBを超える応答でも途中で途切れず、元のバイト列に戻せること
     * （旧・分割送信バグの回帰。base64廃止後はmcp_reply_binが生バイト列を
     * そのまま運ぶので、渡されたバイト数と内容が一致することだけを見る）。 */
    const int n = 16000;
    for (int i = 0; i < n; i++)
        s_output[i] = (uint8_t)(i & 0x7F);
    s_ctx.output_len = n;

    run("TEXT STDOUT");
    assert(s_bin_called);
    assert(s_bin_len == (size_t)n);
    assert(memcmp(s_bin_data, s_output, (size_t)n) == 0);

    /* 送信できたときだけキャプチャを捨てる。 */
    assert(s_ctx.output_len == 0);

    /* 空のキャプチャも同じ経路（0バイトのOK応答）を通る。 */
    run("TEXT STDOUT");
    assert(s_bin_called);
    assert(s_bin_len == 0);

    printf("test_large_reply: OK\n");
}

/* ---- 4. 引数の検証 ----------------------------------------------------- */

static void test_argument_validation(void) {
    assert(strncmp(run("DISK EJECT FD 9"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("SCREENSHOT 9"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("DBG MODE NOPE"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("WP ADD 1000 3"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("BP ADD 1001"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("BP ADD 1000 D0!!!1"), "ERR BADARG ", 11) == 0);

    /* 有効なマスクは mcp_reply_bin 経由でOK＋バイナリ本体を返す。 */
    run("SCREENSHOT 1");
    assert(s_bin_called);
    assert(s_bin_len == 8);

    printf("test_argument_validation: OK\n");
}

/* ---- 5. ブレークポイント・例外マスクの一巡 ----------------------------- */

static void test_breakpoint_lifecycle(void) {
    run("BP CLEAR");
    assert(strncmp(run("BP ADD 1000"), "OK ", 3) == 0);
    int id = atoi(s_reply + 3);
    assert(id >= 0);

    run("BP LIST");
    assert(strncmp(s_reply, "OK 1 ", 5) == 0);
    assert(strstr(s_reply, "001000") != NULL);

    assert(strcmp(run("BP ENABLE 0 0"), "OK\n") == 0);
    assert(strcmp(run("BP DEL 0"), "OK\n") == 0);
    run("BP LIST");
    assert(strncmp(s_reply, "OK 0 ", 5) == 0);

    /* 条件式・パスカウント付きも受け付ける（dbg_cond側の網羅はdebugger_test）。 */
    assert(strncmp(run("BP ADD 2000 D0==12345678 && #3"), "OK ", 3) == 0);
    run("BP CLEAR");

    assert(strcmp(run("EXC SET 000000000000010C"), "OK\n") == 0);
    assert(strcmp(run("EXC GET"), "OK 000000000000010C\n") == 0);
    run("EXC SET 0");

    printf("test_breakpoint_lifecycle: OK\n");
}

/* ---- 6. PASTE キューと逆アセンブル ------------------------------------- */

static void test_paste_and_disasm(void) {
    s_app.key_input_waiting = 1;
    assert(strcmp(run("PASTE 414243"), "OK\n") == 0);
    assert(s_app.key_input_waiting == 0);
    assert(s_app.key_input_paste_active == 1);
    assert(s_ctx.paste_len == 3);
    assert(s_ctx.paste_queue[0] == 'A' && s_ctx.paste_queue[2] == 'C');
    assert(s_ctx.paste_idx == 0);

    for (int i = 0; i < 3; ++i) {
        s_ctx.paste_next_us = 0;
        mcp_commands_poll(s_bus, &s_ctx);
        assert(s_app.key_input_paste_active == (i != 2));
    }

    assert(strcmp(run("KEY CLEAR"), "OK\n") == 0);
    assert(s_app.key_input_paste_active == 0);
    assert(s_ctx.paste_len == 0);

    /* NOP (0x4E71) を置いて DISASM の本文を確認する。 */
    x68k_bus_write_byte(s_bus, 0x003000u, 0x4Eu);
    x68k_bus_write_byte(s_bus, 0x003001u, 0x71u);
    run("DISASM 3000 1");
    uint8_t text[1024];
    int len = decode_reply(text, (int)sizeof(text) - 1);
    text[len] = '\0';
    assert(strstr((char *)text, "003000") != NULL);
    assert(strstr((char *)text, "NOP") != NULL);

    printf("test_paste_and_disasm: OK\n");
}

/* ---- 7. その他の応答書式 ----------------------------------------------- */

static void test_misc_replies(void) {
    /* IOCSのB_KEYINP入口が使う、MFP受信済みバイトの照会契約。UDR/FIFOが
     * 空なら待機に入れるが、どちらかに残っていれば即時入力可能である。 */
    assert(!x68k_bus_mfp_usart_rx_pending(s_bus));
    x68k_bus_mfp_usart_rx_push(s_bus, 0x42);
    assert(x68k_bus_mfp_usart_rx_pending(s_bus));
    x68k_bus_mfp_usart_rx_clear(s_bus);
    assert(!x68k_bus_mfp_usart_rx_pending(s_bus));

    /* KEY PASTE: <キュー残り> <USART未読> <FIFO破棄累計>。FIFO満杯の破棄は件数に出る。 */
    uint32_t dropped = x68k_bus_mfp_usart_rx_dropped(s_bus);
    char expect[64];
    snprintf(expect, sizeof(expect), "OK 0 0 %u\n", (unsigned)dropped);
    assert(strcmp(run("KEY PASTE"), expect) == 0);
    for (int i = 0; i < 40; ++i)
        x68k_bus_mfp_usart_rx_push(s_bus, 0x42);
    assert(x68k_bus_mfp_usart_rx_dropped(s_bus) > dropped);
    snprintf(expect, sizeof(expect), "OK 0 1 %u\n", (unsigned)x68k_bus_mfp_usart_rx_dropped(s_bus));
    assert(strcmp(run("KEY PASTE"), expect) == 0);
    x68k_bus_mfp_usart_rx_clear(s_bus);

    run("REGS");
    assert(strncmp(s_reply, "OK D0=", 6) == 0);
    assert(strstr(s_reply, " SSP=") != NULL);
    assert(s_reply[s_reply_len - 1] == '\n');

    /* HOSTFILEは未ハンドシェイクなら state=0 len=0。 */
    assert(strcmp(run("HOSTFILE STATUS"), "OK 0 0\n") == 0);

    /* DISK_STATUS はユニットごとの行テキストを生バイト列で返す（パスに空白を
     * 含みうるため）。メディア無しは "=" の右が空。 */
    run("DISK STATUS");
    uint8_t disks[1024];
    int dlen = decode_reply(disks, (int)sizeof(disks) - 1);
    disks[dlen] = '\0';
    assert(strstr((char *)disks, "FD0=\n") != NULL);
    assert(strstr((char *)disks, "FD3=\n") != NULL);

    /* KEYWAITは 0/1 の機械可読な1行。 */
    s_app.key_input_waiting = 0;
    assert(strcmp(run("KEY CHECK"), "OK 0\n") == 0);
    s_app.key_input_waiting = 1;
    assert(strcmp(run("KEY CHECK"), "OK 1\n") == 0);
    s_app.key_input_waiting = 0;

    /* DBG_STATUSは実行中ならSTATE=running（regsを含まない別書式、完全一致）。 */
    run("DBG STATUS");
    assert(strcmp(s_reply, "OK STATE=running MODE=AUTO CAP=PC EXEC=NORMAL WB=OFF\n") == 0);

    printf("test_misc_replies: OK\n");
}

/* ---- 7.5. MMU GET（PMMU / access-control レジスタ） -------- */

static void test_mmu_get(void) {
    /* 既定cpuは68000（PMMU非搭載）。0を返すと有効値と区別できなくなるため
     * MMU=UNSUPPORTEDを明示する契約。 */
    assert(strcmp(run("MMU GET"), "OK MODEL=68000 MMU=UNSUPPORTED\n") == 0);

    mc68k_state_t *saved_cpu = s_app.cpu;
    mc68k_bus_t hooks = x68k_bus_get_hooks(s_bus);

    /* 68030: TC/CRP/SRP/TT0/TT1/MMUSRをそのまま返す。CRP/SRPは64bitのまま
     * （下位32bitに切り詰めない）契約を、生成直後の全ゼロ値で確認する。 */
    s_app.cpu = mc68k_create_model(&hooks, MC68K_MODEL_68030);
    assert(s_app.cpu != NULL);
    assert(strcmp(run("MMU GET"),
                  "OK MODEL=68030 TC=00000000 CRP=0000000000000000 SRP=0000000000000000 "
                  "TT0=00000000 TT1=00000000 MMUSR=0000\n") == 0);
    mc68k_destroy(s_app.cpu);

    /* 68EC030: tt0/tt1/mmusrをAC0/AC1/ACUSRの名前で返し、常に0の契約の
     * TC/CRP/SRPは出さない。 */
    s_app.cpu = mc68k_create_model(&hooks, MC68K_MODEL_68EC030);
    assert(s_app.cpu != NULL);
    assert(strcmp(run("MMU GET"), "OK MODEL=68EC030 AC0=00000000 AC1=00000000 ACUSR=0000\n") ==
           0);
    mc68k_destroy(s_app.cpu);

    /* 68040: 030 PMMUとは別の世代別snapshotを返す。TCは16bit、MMUSRは32bitであり、
     * legacy mc68k_mmu_regs_t の表示形式を流用しない。 */
    s_app.cpu = mc68k_create_model(&hooks, MC68K_MODEL_68040);
    assert(s_app.cpu != NULL);
    assert(
        strcmp(run("MMU GET"),
               "OK MODEL=68040 URP=00000000 SRP=00000000 TC=0000 DTT0=00000000 DTT1=00000000 "
               "ITT0=00000000 ITT1=00000000 MMUSR=00000000\n") == 0);
    mc68k_destroy(s_app.cpu);

    s_app.cpu = mc68k_create_model(&hooks, MC68K_MODEL_68060);
    assert(s_app.cpu != NULL);
    assert(strcmp(run("MMU GET"),
                  "OK MODEL=68060 URP=00000000 SRP=00000000 TC=00000000 DTT0=00000000 "
                  "DTT1=00000000 ITT0=00000000 ITT1=00000000\n") == 0);
    mc68k_destroy(s_app.cpu);

    s_app.cpu = saved_cpu;
    printf("test_mmu_get: OK\n");
}

static void test_text_probe_commands(void) {
    run("TEXT EVENTS");
    assert(s_bin_called && s_bin_len == 20 && memcmp(s_bin_data, "TEV1\0\0\0\0", 8) == 0);

    mcp_text_events_reset();
    static const uint8_t payload[] = {'x'};
    for (unsigned i = 0; i < MCP_TEXT_EVENT_MAX + 2; ++i)
        assert(mcp_text_event_emit(i, "test", "overflow", "raw", 0, 0, 0, 0, payload,
                                   sizeof(payload)) == 0);
    run("TEXT EVENTS");
    assert(s_bin_called &&
           s_bin_len == MCP_TEXT_EVENTS_WIRE_HEADER_BYTES +
                            MCP_TEXT_EVENT_MAX *
                                (MCP_TEXT_EVENT_WIRE_METADATA_BYTES + sizeof(payload)));
    assert(s_bin_data[8] == 0 && s_bin_data[9] == 0 && s_bin_data[10] == 0 &&
           s_bin_data[11] == 2);
    run("TEXT EVENTS");
    assert(s_bin_called && s_bin_len == 20 && s_bin_data[8] == 0 && s_bin_data[9] == 0 &&
           s_bin_data[10] == 0 && s_bin_data[11] == 0);

    assert(strcmp(run("TEXT PROBE STATUS"), "OK COUNT=0 ENABLED=0\n") == 0);
    assert(strncmp(run("TEXT PROBE ENABLE"), "ERR NOTREADY ", 13) == 0);
    assert(strncmp(run("TEXT PROBE ADD 1001 STRING D0 16 test probe shift-jis"), "ERR BADARG ",
                   11) == 0);
    assert(strcmp(run("TEXT PROBE ADD A00 STRING D0 16 test putstr shift-jis"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE ADD A02 CHAR S+A 1 test putchar raw"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE STATUS"), "OK COUNT=2 ENABLED=0\n") == 0);
    assert(strncmp(run("TEXT PROBE ADD A04 CHAR D0 1 test bogus raw USER"), "ERR BADARG ", 11) == 0);
    assert(strcmp(run("TEXT PROBE ADD A04 CHAR D0 1 test kernel raw SUPER"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE STATUS"), "OK COUNT=3 ENABLED=0\n") == 0);
    assert(strcmp(run("TEXT PROBE CLEAR"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE ADD A00 STRING D0 16 test putstr shift-jis"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE ADD A02 CHAR S+A 1 test putchar raw"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE ENABLE"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE STATUS"), "OK COUNT=2 ENABLED=1\n") == 0);
    assert(strncmp(run("TEXT PROBE ADD 1004 CHAR D0 1 test later raw"), "ERR NOTREADY ", 13) ==
           0);
    assert(strcmp(run("TEXT PROBE CLEAR"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE STATUS"), "OK COUNT=0 ENABLED=0\n") == 0);
    printf("test_text_probe_commands: OK\n");
}

/* ---- 8. stopinfo（DBG STATUS等）の完全一致・異常系検証 --- */

static void test_stopinfo_full(void) {
    run("BP CLEAR");
    run("WP CLEAR");
    mc68k_regs_t r0;

    /* STEP: BP/WP未登録でDBG STEPすると追加フィールド無しのSTOP=STEPになる
     * （dbg_step()、src/mcp/debugger.c:367-379）。 */
    mc68k_get_regs(s_app.cpu, &r0);
    x68k_bus_write_byte(s_bus, r0.pc, 0x4E);
    x68k_bus_write_byte(s_bus, r0.pc + 1, 0x71); /* NOP */
    run("DBG STEP 1");
    assert_stopinfo("STATE=paused STOP=STEP MODE=AUTO CAP=PC EXEC=NORMAL WB=OFF ");
    dbg_continue();

    /* BREAKPOINT: 現PCにNOPを置き、その次番地にBPを張って1命令進めると、
     * 実行直後のPC判定でヒットする（mc68k_breakpoint_hit()はaddr==pcの
     * 事後判定、extern/cmc68000/src/debug/debugger.c:311-323）。 */
    mc68k_get_regs(s_app.cpu, &r0);
    x68k_bus_write_byte(s_bus, r0.pc, 0x4E);
    x68k_bus_write_byte(s_bus, r0.pc + 1, 0x71); /* NOP */
    char cmd[32], prefix[128];
    snprintf(cmd, sizeof(cmd), "BP ADD %06X", (unsigned)(r0.pc + 2));
    assert(strncmp(run(cmd), "OK ", 3) == 0);
    int bp_id = atoi(s_reply + 3);
    run("DBG STEP 1");
    snprintf(prefix, sizeof(prefix),
             "STATE=paused STOP=BREAKPOINT MODE=AUTO CAP=PC EXEC=DEBUG WB=OFF BP=%d ", bp_id);
    assert_stopinfo(prefix);
    run("BP CLEAR");
    dbg_continue();

    /* WATCHPOINT（24ビットaddr、318バイトケース）: 値変化はポーリング検出
     * のため（dbg_wp_add()のコメント、src/mcp/debugger.h:78-81）、直接
     * メモリを書き換えてから何か1命令実行すれば踏む。 */
    const uint32_t wp_addr = 0x004000u;
    for (uint32_t i = 0; i < 4; i++)
        x68k_bus_write_byte(s_bus, wp_addr + i, 0);
    assert(strncmp(run("WP ADD 4000 4"), "OK ", 3) == 0);
    int wp_id = atoi(s_reply + 3);
    x68k_bus_write_byte(s_bus, wp_addr + 3, 0x12); /* OLD=00000000 -> NEW=00000012 */
    mc68k_get_regs(s_app.cpu, &r0);
    x68k_bus_write_byte(s_bus, r0.pc, 0x4E);
    x68k_bus_write_byte(s_bus, r0.pc + 1, 0x71); /* NOP */
    run("DBG STEP 1");
    snprintf(prefix, sizeof(prefix),
             "STATE=paused STOP=WATCHPOINT MODE=AUTO CAP=PC EXEC=DEBUG WB=OFF WP=%d "
             "WPADDR=%06X OLD=00000000 NEW=00000012 ",
             wp_id, wp_addr);
    assert_stopinfo(prefix);
    run("WP CLEAR");
    dbg_continue();

    /* WATCHPOINT（8桁WPADDR、320バイトケース）: WP ADDのaddrは24ビットへ
     * マスクされずそのまま登録・表示される（mcp_cmd_wp_add()、
     * src/mcp/commands.c:800-815。%06Xは最小幅であって最大幅ではない）。
     * addr上位8bitに0xFFを立てて8桁化しつつ、
     * バス側のマスク（&0xFFFFFFu、mc68k_debug_read_physical_bytes）で
     * 実際に監視される物理番地はRAM上の0x006000になるようにし、実発火まで
     * 検証する（未検証扱いに留めない）。 */
    const uint32_t wp_addr2_raw = 0xFF006000u;
    const uint32_t wp_addr2_phys = wp_addr2_raw & 0xFFFFFFu;
    x68k_bus_write_byte(s_bus, wp_addr2_phys, 0);
    snprintf(cmd, sizeof(cmd), "WP ADD %X 1", wp_addr2_raw);
    assert(strncmp(run(cmd), "OK ", 3) == 0);
    int wp_id2 = atoi(s_reply + 3);
    x68k_bus_write_byte(s_bus, wp_addr2_phys, 0x34); /* OLD=00000000 -> NEW=00000034 */
    mc68k_get_regs(s_app.cpu, &r0);
    x68k_bus_write_byte(s_bus, r0.pc, 0x4E);
    x68k_bus_write_byte(s_bus, r0.pc + 1, 0x71); /* NOP */
    run("DBG STEP 1");
    snprintf(prefix, sizeof(prefix),
             "STATE=paused STOP=WATCHPOINT MODE=AUTO CAP=PC EXEC=DEBUG WB=OFF WP=%d "
             "WPADDR=%08X OLD=00000000 NEW=00000034 ",
             wp_id2, wp_addr2_raw);
    assert_stopinfo(prefix);
    run("WP CLEAR");
    dbg_continue();

    /* USER: DBG BREAKは要求を立てるだけでpaused化しない（paused化は
     * dbg_after_chunk()、src/mcp/debugger.c:308-325）。テストからは
     * dbg_request_break()→dbg_after_chunk()を直接呼ぶ。 */
    dbg_request_break();
    dbg_after_chunk(s_bus);
    run("DBG STATUS");
    assert_stopinfo("STATE=paused STOP=USER MODE=AUTO CAP=PC EXEC=NORMAL WB=OFF ");
    dbg_continue();
    assert(!dbg_is_paused());

    printf("test_stopinfo_full: OK\n");
}

static void test_stopinfo_truncation(void) {
    /* s_dbgをWATCHPOINT（必須6ケース中の最長）でpausedにする。 */
    const uint32_t wp_addr = 0x004000u;
    for (uint32_t i = 0; i < 4; i++)
        x68k_bus_write_byte(s_bus, wp_addr + i, 0);
    assert(strncmp(run("WP ADD 4000 4"), "OK ", 3) == 0);
    x68k_bus_write_byte(s_bus, wp_addr + 3, 0x99);
    mc68k_regs_t r0;
    mc68k_get_regs(s_app.cpu, &r0);
    x68k_bus_write_byte(s_bus, r0.pc, 0x4E);
    x68k_bus_write_byte(s_bus, r0.pc + 1, 0x71); /* NOP */
    run("DBG STEP 1");
    assert(dbg_is_paused());

    /* (1) dbg_format_stopinfo()の戻り値契約: 切り詰め時は「実際に書けた
     * 長さ」ではなく「本来必要だった長さ」を返す（snprintfの戻り値を
     * そのまま使っているため）。256バイトは今回のバグそのもののサイズ。 */
    char small[256];
    memset(small, 'x', sizeof(small));
    int n = dbg_format_stopinfo(small, sizeof(small));
    assert((size_t)n >= sizeof(small));
    assert(memchr(small, '\0', sizeof(small)) != NULL);

    /* (2) mcp_build_stopinfo_reply()の容量判定を直接検証する。 */
    char info[MCP_STOPINFO_MAX];
    char reply[MCP_STOPINFO_MAX + 4];
    assert(mcp_build_stopinfo_reply(info, 64, reply, 68) < 0);          /* infoが足りない */
    assert(mcp_build_stopinfo_reply(info, sizeof(info), reply, 8) < 0); /* replyが足りない */
    int len = mcp_build_stopinfo_reply(info, sizeof(info), reply, sizeof(reply));
    assert(len > 0 && (size_t)len == strlen(reply));
    const char *watchpoint_prefix =
        "OK STATE=paused STOP=WATCHPOINT MODE=AUTO CAP=PC EXEC=DEBUG WB=OFF WP=";
    assert(strncmp(reply, watchpoint_prefix, strlen(watchpoint_prefix)) == 0);

    /* 実際のワイヤ経路（DBG STATUS）も、通常運用ではERR INTERNALへ落ちない。 */
    run("DBG STATUS");
    assert(strncmp(s_reply, "ERR INTERNAL ", 13) != 0);

    run("WP CLEAR");
    dbg_continue();
    assert(!dbg_is_paused());

    printf("test_stopinfo_truncation: OK\n");
}

/* ---- PERF ------------------------------------------------ */

static uint64_t perf_field(const char *name) {
    const char *p = strstr(s_reply, name);
    assert(p != NULL);
    return strtoull(p + strlen(name), NULL, 10);
}

static void perf_write_nops(uint32_t pc, int count) {
    for (int i = 0; i < count; i++) {
        x68k_bus_write_byte(s_bus, pc + (uint32_t)i * 2u, 0x4E);
        x68k_bus_write_byte(s_bus, pc + (uint32_t)i * 2u + 1u, 0x71);
    }
}

/* 測定中に debug-run 経路へ切り替える操作をすると、次の dbg_exec() の先頭で
 * INVALID になり probe が解除される。trigger/undo は ARM 後・後始末のコマンド。 */
static void perf_expect_invalid_on_debug_run(const char *trigger, const char *undo) {
    mc68k_regs_t r;
    mc68k_get_regs(s_app.cpu, &r);
    perf_write_nops(r.pc, 8);
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "PERF ARM %06X %06X", r.pc + 2u, r.pc + 10u);
    assert(strncmp(run(cmd), "OK SEQ=", 7) == 0);
    assert(mcp_perf_owns_probe());
    run(trigger);
    dbg_exec(s_app.cpu, 8);
    assert(dbg_last_exec_used_debug_run() == 1);
    assert(strstr(run("PERF STATUS"), "STATE=INVALID") != NULL);
    assert(!mcp_perf_owns_probe());
    run(undo);
    dbg_continue();
    run("PERF CLEAR");
}

static void test_perf_commands(void) {
    run("BP CLEAR");
    run("WP CLEAR");
    run("PERF CLEAR");
    assert(strncmp(run("PERF STATUS"), "OK SEQ=", 7) == 0);
    assert(strstr(s_reply, "STATE=IDLE") != NULL && strstr(s_reply, "CPU_DELTA=NA") != NULL);

    /* 引数検証: 奇数・同値・非16進・個数違い・引数なしの STATUS/CLEAR。 */
    assert(strncmp(run("PERF ARM 1001 1004"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("PERF ARM 1000 1000"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("PERF ARM 1000 10zz"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("PERF ARM 1000"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("PERF ARM 1000 1004 1008"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("PERF ARM 1000 1000000"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("PERF STATUS x"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("PERF CLEAR x"), "ERR BADARG ", 11) == 0);
    assert(strncmp(run("PERF NOPE"), "ERR UNKNOWN ", 12) == 0);
    assert(!mcp_perf_owns_probe());

    /* debug-run 経路になる設定では ARM を拒否し、probe を登録しない。 */
    run("DBG MODE ON");
    assert(strncmp(run("PERF ARM 1000 1004"), "ERR NOTREADY ", 13) == 0);
    run("DBG MODE AUTO");
    assert(strncmp(run("BP ADD 3000"), "OK ", 3) == 0);
    assert(strncmp(run("PERF ARM 1000 1004"), "ERR NOTREADY ", 13) == 0);
    run("BP CLEAR");
    run("EXC SET 000000000000010C");
    assert(strncmp(run("PERF ARM 1000 1004"), "ERR NOTREADY ", 13) == 0);
    run("EXC SET 0");
    assert(!mcp_perf_owns_probe());

    /* TEXT PROBE との排他: 後発側が失敗し、先行設定は保たれる。 */
    run("TEXT PROBE CLEAR");
    assert(strcmp(run("TEXT PROBE ADD A00 STRING D0 16 test putstr shift-jis"), "OK\n") == 0);
    assert(strcmp(run("TEXT PROBE ENABLE"), "OK\n") == 0);
    assert(strncmp(run("PERF ARM 1000 1004"), "ERR NOTREADY ", 13) == 0);
    assert(strcmp(run("TEXT PROBE STATUS"), "OK COUNT=1 ENABLED=1\n") == 0);
    run("TEXT PROBE CLEAR");
    assert(strncmp(run("PERF ARM 1000 1004"), "OK SEQ=", 7) == 0);
    assert(strstr(s_reply, "STATE=WAIT_START") != NULL);
    assert(strstr(s_reply, "START_PC=001000 END_PC=001004") != NULL);
    assert(strcmp(run("TEXT PROBE ADD A00 STRING D0 16 test putstr shift-jis"), "OK\n") == 0);
    assert(strncmp(run("TEXT PROBE ENABLE"), "ERR NOTREADY ", 13) == 0);
    run("TEXT PROBE CLEAR");

    /* DBG STEP は測定中の区間を INVALID にして probe を解放する。 */
    run("DBG STEP 1");
    dbg_continue();
    assert(strstr(run("PERF STATUS"), "STATE=INVALID") != NULL);
    assert(!mcp_perf_owns_probe());
    run("PERF CLEAR");

    /* 測定中の debug-run 切り替えは INVALID（MODE ON / BP / WP / 例外停止）。 */
    perf_expect_invalid_on_debug_run("DBG MODE ON", "DBG MODE AUTO");
    mc68k_regs_t rb;
    mc68k_get_regs(s_app.cpu, &rb);
    char bp_cmd[32];
    snprintf(bp_cmd, sizeof(bp_cmd), "BP ADD %06X", rb.pc + 0x100u);
    perf_expect_invalid_on_debug_run(bp_cmd, "BP CLEAR");
    perf_expect_invalid_on_debug_run("WP ADD 003000 1", "WP CLEAR");
    perf_expect_invalid_on_debug_run("EXC SET 000000000000010C", "EXC SET 0");

    /* DBG BREAK は要求時点で INVALID（debug-run には入らない）。 */
    {
        mc68k_get_regs(s_app.cpu, &rb);
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "PERF ARM %06X %06X", rb.pc + 2u, rb.pc + 10u);
        assert(strncmp(run(cmd), "OK SEQ=", 7) == 0);
        run("DBG BREAK");
        assert(strstr(run("PERF STATUS"), "STATE=INVALID") != NULL);
        assert(!mcp_perf_owns_probe());
        dbg_continue();
        run("PERF CLEAR");
    }

    /* COMPLETE は後からの debug-run 切り替えで変わらない。 */
    {
        mc68k_get_regs(s_app.cpu, &rb);
        perf_write_nops(rb.pc, 8);
        char cmd[48];
        snprintf(cmd, sizeof(cmd), "PERF ARM %06X %06X", rb.pc, rb.pc + 6u);
        assert(strncmp(run(cmd), "OK SEQ=", 7) == 0);
        int done = dbg_exec(s_app.cpu, 16);
        mcp_perf_account(done, done);
        assert(strstr(run("PERF STATUS"), "STATE=COMPLETE") != NULL);
        run("DBG MODE ON");
        dbg_exec(s_app.cpu, 4);
        assert(strstr(run("PERF STATUS"), "STATE=COMPLETE") != NULL);
        run("DBG MODE AUTO");
        dbg_continue();
        run("PERF CLEAR");
    }

    /* 通常実行の区間計測: 開始 NOP を含み終了 NOP を含まない3命令分。 */
    mc68k_regs_t r;
    mc68k_get_regs(s_app.cpu, &r);
    for (uint32_t i = 0; i < 8; i++) {
        x68k_bus_write_byte(s_bus, r.pc + i * 2u, 0x4E);
        x68k_bus_write_byte(s_bus, r.pc + i * 2u + 1u, 0x71);
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "PERF ARM %06X %06X", r.pc, r.pc + 6u);
    assert(strncmp(run(cmd), "OK SEQ=", 7) == 0);
    int ran = dbg_exec(s_app.cpu, 16);
    assert(ran > 0 && dbg_last_exec_used_debug_run() == 0);
    mcp_perf_account(ran, ran);
    assert(strstr(run("PERF STATUS"), "STATE=COMPLETE") != NULL);
    uint64_t cpu_delta = perf_field("CPU_DELTA=");
    assert(cpu_delta > 0 && cpu_delta == perf_field("ELAPSED_DELTA="));
    assert(perf_field("CPU_END=") - perf_field("CPU_START=") == cpu_delta);
    assert(perf_field("CPU_TOTAL=") >= perf_field("CPU_END="));
    assert(strcmp(run("PERF CLEAR"), "OK\n") == 0);
    assert(strstr(run("PERF STATUS"), "STATE=IDLE") != NULL);

    printf("test_perf_commands: OK\n");
}

/* ---- 9. PROTO v4 stopinfoの状態フィールド ------- */

static void test_stopinfo_v4_fields(void) {
    /* 実際のコアring状態からCAPを作る。WHITEBAND等の副作用をローカル状態で
     * 推測しないこと、およびEXECがringだけではDEBUG化しないことを固定する。 */
    run("BP CLEAR");
    run("WP CLEAR");
    run("EXC SET 0");
    run("DBG MODE AUTO");
    run("DIAG PCRING OFF");
    run("DIAG REGRING OFF");
    assert(strcmp(run("DBG STATUS"),
                  "OK STATE=running MODE=AUTO CAP=NONE EXEC=NORMAL WB=OFF\n") == 0);

    run("DIAG PCRING ON");
    assert(strcmp(run("DBG STATUS"),
                  "OK STATE=running MODE=AUTO CAP=PC EXEC=NORMAL WB=OFF\n") == 0);
    run("DIAG REGRING ON");
    assert(strcmp(run("DBG STATUS"),
                  "OK STATE=running MODE=AUTO CAP=PC,REG EXEC=NORMAL WB=OFF\n") == 0);
    run("DIAG PCRING OFF");
    assert(strcmp(run("DBG STATUS"),
                  "OK STATE=running MODE=AUTO CAP=REG EXEC=NORMAL WB=OFF\n") == 0);

    /* BPはAUTOではDEBUG経路を選ぶが、MODE OFFはBPが残っていてもNORMALを
     * 強制する。これはdbg_exec()と同じ条件をstopinfoが表している回帰。 */
    assert(strncmp(run("BP ADD 1000"), "OK ", 3) == 0);
    assert(strcmp(run("DBG STATUS"),
                  "OK STATE=running MODE=AUTO CAP=REG EXEC=DEBUG WB=OFF\n") == 0);
    run("DBG MODE OFF");
    assert(strcmp(run("DBG STATUS"),
                  "OK STATE=running MODE=OFF CAP=REG EXEC=NORMAL WB=OFF\n") == 0);
    run("DBG MODE ON");
    assert(strcmp(run("DBG STATUS"), "OK STATE=running MODE=ON CAP=REG EXEC=DEBUG WB=OFF\n") ==
           0);
    run("BP CLEAR");
    run("DBG MODE AUTO");
    run("DIAG REGRING OFF");

    /* CPU未初期化ではringも実行経路も存在しない。MODE ONの設定が残って
     * いても、CAP=NONE/EXEC=NORMALを返すv4契約を直接検証する。 */
    mc68k_state_t *saved_cpu = s_app.cpu;
    s_app.cpu = NULL;
    run("DBG MODE ON");
    assert(strcmp(run("DBG STATUS"),
                  "OK STATE=running MODE=ON CAP=NONE EXEC=NORMAL WB=OFF\n") == 0);
    s_app.cpu = saved_cpu;
    run("DBG MODE AUTO");
    printf("test_stopinfo_v4_fields: OK\n");
}

/* ---- 10. checkpoint wire command ------------------------------------- */

static void test_snapshot_commands(void) {
    assert(strcmp(run("SNAPSHOT STATUS"), "OK SLOT=EMPTY\n") == 0);
    assert(strcmp(run("SNAPSHOT SAVE"), "ERR NOTREADY RUNNING\n") == 0);
    assert(strcmp(run("SNAPSHOT SAVE extra"), "ERR BADARG SNAPSHOT SAVE\n") == 0);

    dbg_halt_whiteband();
    assert(strcmp(run("SNAPSHOT SAVE"), "OK SLOT=VALID\n") == 0);
    assert(strcmp(run("SNAPSHOT STATUS"), "OK SLOT=VALID\n") == 0);

    s_ctx.output_len = 12;
    s_ctx.wait_kind = MCP_WAIT_DBG;
    const char *reply = run("SNAPSHOT LOAD");
    assert(strstr(reply, "OK STATE=paused STOP=SNAPSHOT ") == reply);
    assert(strstr(reply, "RINGS=CLEARED ") != NULL);
    assert(s_ctx.output_len == 0);
    assert(s_ctx.wait_kind == MCP_WAIT_NONE);
    assert(strcmp(run("SNAPSHOT LOAD extra"), "ERR BADARG SNAPSHOT LOAD\n") == 0);
    dbg_continue();
    printf("test_snapshot_commands: OK\n");
}

/* ---- 11. 非同期待ちの共通機構（KEY WAIT / DBG WAIT） ------------------- */

static void test_async_wait(void) {
    /* 完了済みなら即応答し、待ちは持ち越さない。 */
    s_app.key_input_waiting = 1;
    assert(strcmp(run("KEY WAIT 1000"), "OK 1\n") == 0);
    assert(s_ctx.wait_kind == MCP_WAIT_NONE);

    /* 未完了なら応答せず、pollへ持ち越す。 */
    s_app.key_input_waiting = 0;
    s_reply_len = 0;
    s_reply[0] = '\0';
    assert(mcp_commands_handle(s_bus, &s_ctx, "KEY WAIT 1000") == 1);
    assert(s_reply_len == 0);
    assert(s_ctx.wait_kind == MCP_WAIT_KEYIN);

    /* pollで条件が成立したら、そこで応答して待ちを終える。 */
    s_app.key_input_waiting = 1;
    mcp_commands_poll(s_bus, &s_ctx);
    assert(strcmp(s_reply, "OK 1\n") == 0);
    assert(s_ctx.wait_kind == MCP_WAIT_NONE);
    s_app.key_input_waiting = 0;

    /* 期限切れは異常ではないので、コード付きのTIMEOUTで返す。 */
    s_reply_len = 0;
    s_reply[0] = '\0';
    mcp_commands_handle(s_bus, &s_ctx, "KEY WAIT 0");
    assert(s_ctx.wait_kind == MCP_WAIT_KEYIN);
    mcp_commands_poll(s_bus, &s_ctx);
    assert(strncmp(s_reply, "ERR TIMEOUT ", 12) == 0);
    assert(s_ctx.wait_kind == MCP_WAIT_NONE);

    /* 切断時は待ちを捨てる（同じ機構をDBG WAITも通る）。 */
    mcp_commands_handle(s_bus, &s_ctx, "DBG WAIT 1000");
    assert(s_ctx.wait_kind == MCP_WAIT_DBG);
    mcp_commands_client_disconnected(&s_ctx);
    assert(s_ctx.wait_kind == MCP_WAIT_NONE);

    printf("test_async_wait: OK\n");
}

int main(void) {
    setup();
    test_dispatch();
    test_memory();
    test_large_reply();
    test_argument_validation();
    test_breakpoint_lifecycle();
    test_paste_and_disasm();
    test_misc_replies();
    test_mmu_get();
    test_text_probe_commands();
    test_perf_commands();
    test_stopinfo_full();
    test_stopinfo_truncation();
    test_stopinfo_v4_fields();
    test_snapshot_commands();
    test_async_wait();
    printf("mcp_commands_test: 全テストOK\n");
    return 0;
}
