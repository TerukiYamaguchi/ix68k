/*
 * debugger.c — MCP デバッガ実行制御の実装。
 */
#include "mcp/debugger.h"
#include "mcp/dbg_cond.h"
#include "mcp/perf.h"
#include "app_internal.h"
#include "platform/platform.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    DBG_STOP_NONE = 0,
    DBG_STOP_USER, /* DBG_BREAKによる手動停止 */
    DBG_STOP_BREAKPOINT,
    DBG_STOP_WATCHPOINT,
    DBG_STOP_TRACE,
    DBG_STOP_EXCEPTION,
    DBG_STOP_HALTED,
    DBG_STOP_STEP,      /* DBG_STEP完了時、他に停止理由が無ければコレ */
    DBG_STOP_WHITEBAND, /* Human68kの白帯（_ABORTRST $FD）検出 */
    DBG_STOP_SNAPSHOT,
} dbg_stop_kind_t;

static struct {
    int bp_count;
    int wp_count;
    /* 例外ブレークマスクはホスト側に複製せずmc68k_get_exception_break_mask()を
     * 都度問い合わせる（dbg_needs_debug_run()参照）。ここにフィールドは持たない。 */
    dbg_mode_t force_mode;
    int paused;
    int break_requested;
    /* 直近チャンクがmc68k_debug_run()経由だったか。mc68k_execute()は
     * stop_reasonを更新しないため（debug.h参照）、この直後だけ
     * mc68k_get_stop_reason()を信用してよい。falseのまま読むと、過去の
     * debug_run()呼び出しの古い値を誤って現在の停止理由と解釈しかねない。 */
    int last_used_debug_run;
    dbg_stop_kind_t stop_kind;
    int stop_bp_id; /* stop_kind==DBG_STOP_BREAKPOINT時のヒットID。それ以外は-1 */
    int stop_wp_id; /* stop_kind==DBG_STOP_WATCHPOINT時のヒットID。それ以外は-1 */
    uint32_t stop_wp_addr;
    uint32_t stop_wp_old;
    uint32_t stop_wp_new;
} s_dbg;

/* ブレークポイントのホスト側メタデータ。コアの mc68k_add_breakpoint() が
 * 返すID＝配列インデックスそのものと1:1対応させる（debug.h参照）。 */
typedef struct {
    int in_use;
    uint32_t addr;
    int enabled;
    int hits;
    int has_cond;
    char cond_text[64]; /* BP_LIST表示用の元テキスト（has_cond=0なら空文字列） */
    dbg_cond_t cond;
} dbg_bp_t;

static dbg_bp_t s_bp[MC68K_MAX_BREAKPOINTS];

static void dbg_set_paused(int paused) {
    if (s_dbg.paused == paused)
        return;
    s_dbg.paused = paused;
    if (s_app.bus)
        x68k_bus_guest_clock_set_paused(s_app.bus, paused);
}

/*
 * 条件付きブレークポイント全件で共用する単一コールバック（debug.hの
 * mc68k_bp_cond_fn_t契約）。mc68k_add_breakpoint()はIDを戻り値でしか
 * 教えてくれず、登録呼び出し時点ではまだ自分のs_bp[id]を特定できないため
 * （ctxに渡すポインタを先に決められない）、cond_ctxは使わずcpu側が既に
 * 確定させた現在PCで該当スロットを逆引きする。呼ばれる時点でPC==該当BPの
 * addrであることはコア側の判定順序（アドレス一致→cond呼び出し）で保証済み。
 */
static int dbg_bp_cond_cb(mc68k_state_t *cpu, void *ctx) {
    (void)ctx;
    uint32_t pc = mc68k_get_pc(cpu);
    for (int i = 0; i < MC68K_MAX_BREAKPOINTS; i++) {
        if (s_bp[i].in_use && s_bp[i].has_cond && s_bp[i].addr == pc)
            return dbg_cond_eval(&s_bp[i].cond, cpu);
    }
    return 0;
}

int dbg_bp_add(uint32_t addr, const char *cond_text) {
    if (!s_app.cpu)
        return -1;
    if (addr & 1u)
        return -3;
    dbg_cond_t parsed;
    int has_cond = (cond_text && cond_text[0] != '\0');
    if (has_cond && dbg_cond_parse(cond_text, &parsed) != 0)
        return -2;
    int id = mc68k_add_breakpoint(s_app.cpu, addr, has_cond ? dbg_bp_cond_cb : NULL, NULL);
    if (id < 0)
        return -1;
    dbg_bp_t *bp = &s_bp[id];
    bp->in_use = 1;
    bp->addr = addr;
    bp->enabled = 1;
    bp->hits = 0;
    bp->has_cond = has_cond;
    if (has_cond) {
        bp->cond = parsed;
        snprintf(bp->cond_text, sizeof(bp->cond_text), "%s", cond_text);
    } else {
        bp->cond_text[0] = '\0';
    }
    s_dbg.bp_count++;
    return id;
}

void dbg_bp_del(int id) {
    if (id < 0 || id >= MC68K_MAX_BREAKPOINTS || !s_bp[id].in_use || !s_app.cpu)
        return;
    mc68k_remove_breakpoint(s_app.cpu, id);
    s_bp[id].in_use = 0;
    s_dbg.bp_count--;
}

void dbg_bp_set_enable(int id, int enable) {
    if (id < 0 || id >= MC68K_MAX_BREAKPOINTS || !s_bp[id].in_use || !s_app.cpu)
        return;
    mc68k_enable_breakpoint(s_app.cpu, id, enable);
    s_bp[id].enabled = enable ? 1 : 0;
}

void dbg_bp_clear(void) {
    if (s_app.cpu)
        mc68k_clear_breakpoints(s_app.cpu);
    for (int i = 0; i < MC68K_MAX_BREAKPOINTS; i++)
        s_bp[i].in_use = 0;
    s_dbg.bp_count = 0;
}

int dbg_bp_format_list(char *buf, size_t bufsz, int *out_count) {
    int n = 0;
    size_t off = 0;
    for (int i = 0; i < MC68K_MAX_BREAKPOINTS; i++) {
        if (!s_bp[i].in_use)
            continue;
        int w = snprintf(buf + off, bufsz - off, "%sid=%d,addr=%06X,en=%d,hits=%d,cond=%s",
                         n ? ";" : "", i, s_bp[i].addr, s_bp[i].enabled, s_bp[i].hits,
                         s_bp[i].cond_text);
        if (w < 0 || (size_t)w >= bufsz - off)
            break;
        off += (size_t)w;
        n++;
    }
    if (out_count)
        *out_count = n;
    return (int)off;
}

/* ウォッチポイントのホスト側メタデータ。BP と同じくコアの
 * mc68k_add_watchpoint() が返すID＝配列インデックスそのものと1:1対応させる。 */
typedef struct {
    int in_use;
    uint32_t addr;
    int size; /* 1/2/4 */
    int enabled;
    int hits;
    uint32_t last_old;
    uint32_t last_new;
} dbg_wp_t;

static dbg_wp_t s_wp[MC68K_MAX_WATCHPOINTS];

int dbg_wp_add(uint32_t addr, int size) {
    if (!s_app.cpu)
        return -1;
    if (size != 1 && size != 2 && size != 4)
        return -2;
    int id = mc68k_add_watchpoint(s_app.cpu, addr, size);
    if (id < 0)
        return -1; /* 登録数上限・奇数addr・peek未登録のいずれか（コア側で区別されない） */
    dbg_wp_t *wp = &s_wp[id];
    wp->in_use = 1;
    wp->addr = addr;
    wp->size = size;
    wp->enabled = 1;
    wp->hits = 0;
    wp->last_old = 0;
    wp->last_new = 0;
    s_dbg.wp_count++;
    return id;
}

void dbg_wp_del(int id) {
    if (id < 0 || id >= MC68K_MAX_WATCHPOINTS || !s_wp[id].in_use || !s_app.cpu)
        return;
    mc68k_remove_watchpoint(s_app.cpu, id);
    s_wp[id].in_use = 0;
    s_dbg.wp_count--;
}

void dbg_wp_set_enable(int id, int enable) {
    if (id < 0 || id >= MC68K_MAX_WATCHPOINTS || !s_wp[id].in_use || !s_app.cpu)
        return;
    mc68k_enable_watchpoint(s_app.cpu, id, enable);
    s_wp[id].enabled = enable ? 1 : 0;
}

void dbg_wp_clear(void) {
    if (s_app.cpu)
        mc68k_clear_watchpoints(s_app.cpu);
    for (int i = 0; i < MC68K_MAX_WATCHPOINTS; i++)
        s_wp[i].in_use = 0;
    s_dbg.wp_count = 0;
}

int dbg_wp_format_list(char *buf, size_t bufsz, int *out_count) {
    int n = 0;
    size_t off = 0;
    for (int i = 0; i < MC68K_MAX_WATCHPOINTS; i++) {
        if (!s_wp[i].in_use)
            continue;
        int w = snprintf(buf + off, bufsz - off,
                         "%sid=%d,addr=%06X,size=%d,en=%d,hits=%d,last=%0*X", n ? ";" : "", i,
                         s_wp[i].addr, s_wp[i].size, s_wp[i].enabled, s_wp[i].hits,
                         s_wp[i].size * 2, s_wp[i].last_new);
        if (w < 0 || (size_t)w >= bufsz - off)
            break;
        off += (size_t)w;
        n++;
    }
    if (out_count)
        *out_count = n;
    return (int)off;
}

/*
 * 例外ブレークマスクはホスト側に複製せず、常に
 * コアへ直接パススルーする（mc68k_reset()を跨いでコア側が保持する値を
 * 唯一の正とする。ホスト側キャッシュを持つと同期漏れの温床になるため）。
 */
void dbg_set_exc_mask(uint64_t mask) {
    if (s_app.cpu)
        mc68k_set_exception_break_mask(s_app.cpu, mask);
}

uint64_t dbg_get_exc_mask(void) {
    return s_app.cpu ? mc68k_get_exception_break_mask(s_app.cpu) : 0;
}

static int dbg_needs_debug_run(void) {
    return s_dbg.bp_count > 0 || s_dbg.wp_count > 0 || dbg_get_exc_mask() != 0 ||
           s_dbg.force_mode == DBG_MODE_ON;
}

/* 次の通常実行チャンクが命令単位のdebug_run経路になるか。stopinfoのEXECは
 * 実際のdbg_exec()と同じ判定を表すため、ここへ切り出して二重定義を避ける。 */
static int dbg_uses_debug_run(void) {
    return s_app.cpu && s_dbg.force_mode != DBG_MODE_OFF && dbg_needs_debug_run();
}

int dbg_next_exec_uses_debug_run(void) {
    return dbg_uses_debug_run();
}

int dbg_exec(mc68k_state_t *cpu, int chunk) {
    if (dbg_uses_debug_run()) {
        /* debug_run 経路の区間測定は比較対象外。 */
        mcp_perf_invalidate();
        s_dbg.last_used_debug_run = 1;
        return mc68k_debug_run(cpu, chunk);
    }
    s_dbg.last_used_debug_run = 0;
    int ran = mc68k_execute(cpu, chunk);
    mcp_perf_after_exec(); /* entry-probe callback内では解除できないため復帰後に行う */
    return ran;
}

int dbg_last_exec_used_debug_run(void) {
    return s_dbg.last_used_debug_run;
}

static dbg_stop_kind_t dbg_kind_from_stop_reason(mc68k_stop_reason_t r) {
    switch (r) {
    case MC68K_STOP_BREAKPOINT:
        return DBG_STOP_BREAKPOINT;
    case MC68K_STOP_WATCHPOINT:
        return DBG_STOP_WATCHPOINT;
    case MC68K_STOP_TRACE:
        return DBG_STOP_TRACE;
    case MC68K_STOP_EXCEPTION:
        return DBG_STOP_EXCEPTION;
    case MC68K_STOP_HALTED:
        return DBG_STOP_HALTED;
    case MC68K_STOP_NONE:
    default:
        return DBG_STOP_NONE;
    }
}

/*
 * 停止理由rをラッチしてpaused状態にする共通処理。r==MC68K_STOP_NONEなら
 * fallback_kindを使う（dbg_step()がBP/WP等にヒットせず正常にn命令完了した
 * 場合のDBG_STOP_STEP用）。BREAKPOINTヒット時のID取得とhits記録も
 * ここに集約する（dbg_after_chunk()とdbg_step()の共通処理）。
 */
static void dbg_latch_stop(mc68k_stop_reason_t r, dbg_stop_kind_t fallback_kind) {
    s_dbg.stop_kind = (r != MC68K_STOP_NONE) ? dbg_kind_from_stop_reason(r) : fallback_kind;
    s_dbg.stop_bp_id = -1;
    s_dbg.stop_wp_id = -1;
    if (s_dbg.stop_kind == DBG_STOP_BREAKPOINT) {
        /* mc68k_breakpoint_hit()の呼び出し順序規約（debug.h）:「1命令実行→
         * その直後にmc68k_breakpoint_hit()でPC判定」を満たす。
         * mc68k_debug_run()/debug_run_n()が停止した直後のここではPCは
         * ヒットした瞬間のままで、間に他の命令は実行されていない。 */
        int id = mc68k_breakpoint_hit(s_app.cpu);
        s_dbg.stop_bp_id = id;
        if (id >= 0 && id < MC68K_MAX_BREAKPOINTS && s_bp[id].in_use)
            s_bp[id].hits++;
    } else if (s_dbg.stop_kind == DBG_STOP_WATCHPOINT) {
        /* mc68k_get_watchpoint_hit()もBPと同じ「直後に呼ぶ」規約
         * （debug.h、値変化検出はエッジトリガのため呼び出しタイミングが重要）。 */
        uint32_t old_v = 0, new_v = 0;
        int id = mc68k_get_watchpoint_hit(s_app.cpu, &old_v, &new_v);
        s_dbg.stop_wp_id = id;
        s_dbg.stop_wp_old = old_v;
        s_dbg.stop_wp_new = new_v;
        if (id >= 0 && id < MC68K_MAX_WATCHPOINTS && s_wp[id].in_use) {
            s_dbg.stop_wp_addr = s_wp[id].addr;
            s_wp[id].hits++;
            s_wp[id].last_old = old_v;
            s_wp[id].last_new = new_v;
        }
    }
    dbg_set_paused(1);
}

int dbg_after_chunk(x68k_bus_t *bus) {
    (void)bus; /* 停止時の追加ログ出力用に予約 */
    if (s_dbg.paused)
        return 1;
    if (s_dbg.last_used_debug_run && s_app.cpu) {
        mc68k_stop_reason_t r = mc68k_get_stop_reason(s_app.cpu);
        if (r != MC68K_STOP_NONE) {
            dbg_latch_stop(r, DBG_STOP_NONE); /* r!=NONE保証済みのためfallback未使用 */
            return 1;
        }
    }
    if (s_dbg.break_requested) {
        s_dbg.break_requested = 0;
        s_dbg.stop_kind = DBG_STOP_USER;
        dbg_set_paused(1);
        return 1;
    }
    return 0;
}

int dbg_is_paused(void) {
    return s_dbg.paused;
}

void dbg_set_mode(dbg_mode_t mode) {
    s_dbg.force_mode = mode;
}

const char *dbg_mode_name(void) {
    switch (s_dbg.force_mode) {
    case DBG_MODE_ON:
        return "ON";
    case DBG_MODE_OFF:
        return "OFF";
    case DBG_MODE_AUTO:
    default:
        return "AUTO";
    }
}

void dbg_continue(void) {
    dbg_set_paused(0);
    s_dbg.break_requested = 0; /* CONT直前の未消化のBREAK要求も一緒に取り消す */
    s_dbg.stop_kind = DBG_STOP_NONE;
    s_dbg.stop_bp_id = -1;
    s_dbg.stop_wp_id = -1;
}

void dbg_request_break(void) {
    mcp_perf_invalidate();
    s_dbg.break_requested = 1;
}

void dbg_halt_whiteband(void) {
    s_dbg.stop_kind = DBG_STOP_WHITEBAND;
    s_dbg.stop_bp_id = -1;
    s_dbg.stop_wp_id = -1;
    dbg_set_paused(1);
}

void dbg_enter_snapshot_stop(void) {
    mcp_perf_invalidate();
    s_dbg.stop_kind = DBG_STOP_SNAPSHOT;
    s_dbg.stop_bp_id = -1;
    s_dbg.stop_wp_id = -1;
    s_dbg.break_requested = 0;
    s_dbg.last_used_debug_run = 0;
    dbg_set_paused(1);
}

void dbg_wp_rearm_all(void) {
    if (s_app.cpu)
        mc68k_rearm_watchpoints(s_app.cpu);
}

int dbg_step(x68k_bus_t *bus, int n) {
    if (!s_app.cpu)
        return 0;
    if (n < 1)
        n = 1;
    if (n > 65536) /* 暴走防止の上限（設計書に明記なし、安全側の実装判断） */
        n = 65536;
    mcp_perf_invalidate();
    int ran = mc68k_debug_run_n(s_app.cpu, n);
    x68k_cycle_catchup(bus, ran);
    mcp_perf_account(ran, ran > 0 ? ran : 1); /* x68k_cycle_catchup() が周辺へ進めた量と同じ */
    mc68k_stop_reason_t r = mc68k_get_stop_reason(s_app.cpu);
    dbg_latch_stop(r, DBG_STOP_STEP);
    s_dbg.break_requested = 0;
    return ran;
}

static const char *dbg_stop_kind_name(dbg_stop_kind_t k) {
    switch (k) {
    case DBG_STOP_USER:
        return "USER";
    case DBG_STOP_BREAKPOINT:
        return "BREAKPOINT";
    case DBG_STOP_WATCHPOINT:
        return "WATCHPOINT";
    case DBG_STOP_TRACE:
        return "TRACE";
    case DBG_STOP_EXCEPTION:
        return "EXCEPTION";
    case DBG_STOP_HALTED:
        return "HALTED";
    case DBG_STOP_STEP:
        return "STEP";
    case DBG_STOP_WHITEBAND:
        return "WHITEBAND";
    case DBG_STOP_SNAPSHOT:
        return "SNAPSHOT";
    case DBG_STOP_NONE:
    default:
        return "NONE";
    }
}

/* 白帯検出HALTの armed 状態。WHITEBAND ON はプロセス状態なので、停止しない
 * のが「ONにし忘れ」なのか「白帯が出ていない」のかを状態表示で切り分けられる
 * ようにする。 */
static const char *dbg_whiteband_state_name(void) {
    return s_app.whiteband_halt_on ? "ON" : "OFF";
}

static const char *dbg_capture_state_name(void) {
    int pc = mc68k_get_pc_ring(s_app.cpu);
    int reg = mc68k_get_reg_ring(s_app.cpu);
    if (pc && reg)
        return "PC,REG";
    if (pc)
        return "PC";
    if (reg)
        return "REG";
    return "NONE";
}

static const char *dbg_exec_state_name(void) {
    return dbg_uses_debug_run() ? "DEBUG" : "NORMAL";
}

int dbg_format_stopinfo(char *buf, size_t bufsz) {
    if (!s_dbg.paused)
        return snprintf(buf, bufsz, "STATE=running MODE=%s CAP=%s EXEC=%s WB=%s",
                        dbg_mode_name(), dbg_capture_state_name(), dbg_exec_state_name(),
                        dbg_whiteband_state_name());
    mc68k_regs_t r;
    memset(&r, 0, sizeof(r));
    if (s_app.cpu)
        mc68k_get_regs(s_app.cpu, &r);
    int n =
        snprintf(buf, bufsz, "STATE=paused STOP=%s MODE=%s CAP=%s EXEC=%s WB=%s ",
                 dbg_stop_kind_name(s_dbg.stop_kind), dbg_mode_name(), dbg_capture_state_name(),
                 dbg_exec_state_name(), dbg_whiteband_state_name());
    if (n < 0 || (size_t)n >= bufsz)
        return n;
    if (s_dbg.stop_kind == DBG_STOP_SNAPSHOT) {
        int w = snprintf(buf + n, bufsz - (size_t)n, "RINGS=CLEARED ");
        if (w < 0 || (size_t)(n + w) >= bufsz)
            return n + (w < 0 ? 0 : w);
        n += w;
    } else if (s_dbg.stop_kind == DBG_STOP_BREAKPOINT) {
        int w = snprintf(buf + n, bufsz - (size_t)n, "BP=%d ", s_dbg.stop_bp_id);
        if (w < 0 || (size_t)(n + w) >= bufsz)
            return n + (w < 0 ? 0 : w);
        n += w;
    } else if (s_dbg.stop_kind == DBG_STOP_WATCHPOINT) {
        int w = snprintf(buf + n, bufsz - (size_t)n, "WP=%d WPADDR=%06X OLD=%08X NEW=%08X ",
                         s_dbg.stop_wp_id, s_dbg.stop_wp_addr, s_dbg.stop_wp_old,
                         s_dbg.stop_wp_new);
        if (w < 0 || (size_t)(n + w) >= bufsz)
            return n + (w < 0 ? 0 : w);
        n += w;
    } else if (s_dbg.stop_kind == DBG_STOP_TRACE) {
        /* トレース例外は常にベクタ9固定（debug.hのMC68K_STOP_TRACE定義通り）。 */
        int w = snprintf(buf + n, bufsz - (size_t)n, "VEC=9 ");
        if (w < 0 || (size_t)(n + w) >= bufsz)
            return n + (w < 0 ? 0 : w);
        n += w;
    } else if (s_dbg.stop_kind == DBG_STOP_EXCEPTION) {
        int vec = mc68k_get_last_exception_vector(s_app.cpu);
        if (vec >= 0) {
            int w = snprintf(buf + n, bufsz - (size_t)n, "VEC=%d ", vec);
            if (w < 0 || (size_t)(n + w) >= bufsz)
                return n + (w < 0 ? 0 : w);
            n += w;
        }
    }
    int w =
        snprintf(buf + n, bufsz - (size_t)n,
                 "PC=%08X "
                 "D0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X "
                 "A0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X "
                 "SR=%04X USP=%08X SSP=%08X",
                 r.pc, r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6], r.d[7], r.a[0],
                 r.a[1], r.a[2], r.a[3], r.a[4], r.a[5], r.a[6], r.a[7], r.sr, r.usp, r.ssp);
    return (w < 0) ? n : n + w;
}

/*
 * ログハンドラ・ダンプ系。
 * mc68k_set_log_handler()を常時登録し、既定（非キャプチャ中）はpal_log()へ
 * そのまま流す（現行のstderr出力と等価）。ダンプコマンド実行中だけキャプチャ
 * フラグを立て、テキストを固定長バッファへ溜めてから呼び出し元へ返す。
 */
static struct {
    int capturing;
    size_t len;
    char buf[DBG_LOG_CAP_SIZE];
} s_log_cap;

static void dbg_log_cb(void *ctx, const char *msg) {
    (void)ctx;
    if (!s_log_cap.capturing) {
        pal_log("%s", msg);
        return;
    }
    size_t mlen = strlen(msg);
    size_t avail = DBG_LOG_CAP_SIZE - 1 - s_log_cap.len; /* NUL終端分を残す */
    if (mlen > avail)
        mlen = avail;
    if (mlen > 0) {
        memcpy(s_log_cap.buf + s_log_cap.len, msg, mlen);
        s_log_cap.len += mlen;
    }
}

void dbg_init_log_handler(void) {
    if (s_app.cpu)
        mc68k_set_log_handler(s_app.cpu, dbg_log_cb, NULL);
}

/* dump_fn呼び出し中だけログをキャプチャしbufへコピーする共通処理
 * （dbg_dump_call_ring/pc_ring/reg_ring/fault_ringで共用）。 */
static int dbg_capture_dump(void (*dump_fn)(const mc68k_state_t *, int), int count, char *buf,
                            size_t bufsz) {
    if (!s_app.cpu || !buf || bufsz == 0)
        return -1;
    s_log_cap.capturing = 1;
    s_log_cap.len = 0;
    dump_fn(s_app.cpu, count);
    s_log_cap.capturing = 0;
    size_t n = s_log_cap.len;
    if (n >= bufsz)
        n = bufsz - 1;
    memcpy(buf, s_log_cap.buf, n);
    buf[n] = '\0';
    return (int)n;
}

int dbg_dump_call_ring(char *buf, size_t bufsz, int count) {
    return dbg_capture_dump(mc68k_dump_call_ring, count, buf, bufsz);
}

int dbg_dump_pc_ring(char *buf, size_t bufsz, int count) {
    return dbg_capture_dump(mc68k_dump_pc_ring, count, buf, bufsz);
}

int dbg_dump_reg_ring(char *buf, size_t bufsz, int count) {
    return dbg_capture_dump(mc68k_dump_reg_ring, count, buf, bufsz);
}

int dbg_dump_fault_ring(char *buf, size_t bufsz, int count) {
    return dbg_capture_dump(mc68k_dump_fault_ring, count, buf, bufsz);
}

void dbg_set_badjump_log(int on) {
    if (s_app.cpu)
        mc68k_set_badjump_log(s_app.cpu, on);
}

void dbg_set_badmovea_log(int on) {
    if (s_app.cpu)
        mc68k_set_badmovea_log(s_app.cpu, on);
}

void dbg_set_badaddr_log(int on) {
    if (s_app.cpu)
        mc68k_set_badaddr_log(s_app.cpu, on);
}

void dbg_set_badori_log(int on) {
    if (s_app.cpu)
        mc68k_set_badori_log(s_app.cpu, on);
}
