#pragma once
/*
 * debugger.h — MCP デバッガ実行制御。
 *
 * main.c（チャンクループ）とmcp/commands.c（DBG_*コマンド）の両方から使う。
 * CPUハンドルは自前保持せずapp_internal.hのs_app.cpuグローバルを参照する
 * （x68k_cycle_catchup()と同じ流儀）。
 */
#include <stddef.h>
#include <stdint.h>
#include "bus/x68k_bus.h"
#include "mc68000/cpu_types.h"

typedef enum { DBG_MODE_AUTO = 0, DBG_MODE_ON, DBG_MODE_OFF } dbg_mode_t;

/* ダンプ系（CALLRING/PCRING/REGRING/FAULTRING）のキャプチャバッファサイズ。 */
#define DBG_LOG_CAP_SIZE (64 * 1024)

/*
 * main.cのチャンクループから呼ぶ。
 *
 * dbg_exec(): mc68k_execute()の代替。BP/WP/例外マスクが登録されているか
 *   DBG_MODE ON時のみmc68k_debug_run()に切り替える「autoモード」。それ以外は
 *   mc68k_execute()を直接呼ぶゼロコスト経路。DBG_MODE OFFは常に
 *   mc68k_execute()を強制する逃げ道。
 * dbg_after_chunk(): チャンク実行直後に停止判定する。停止したら1を返し、以後
 *   dbg_is_paused()が真になる（DBG_CONTまで）。bus は将来のログ出力用に予約。
 */
int dbg_exec(mc68k_state_t *cpu, int chunk);

/* 次の dbg_exec() が mc68k_debug_run() 経路になるなら1（PERF ARM の拒否判定に使う）。 */
int dbg_next_exec_uses_debug_run(void);

/* 直近の dbg_exec() が mc68k_debug_run() 経由なら1。この経路では
 * mc68k_executed_cycles() が命令ごとに0へ戻るため、MFPのチャンク内同期には使えない。 */
int dbg_last_exec_used_debug_run(void);
int dbg_after_chunk(x68k_bus_t *bus);
int dbg_is_paused(void);

/* mcp/commands.cのDBG_*コマンドハンドラから呼ぶ。 */
void dbg_set_mode(dbg_mode_t mode);
const char *dbg_mode_name(void);
void dbg_continue(void);      /* DBG_CONT: 停止解除（次チャンクから再開） */
void dbg_request_break(void); /* DBG_BREAK: 次チャンク境界での停止を要求 */

/* 白帯検出（IOCS _ABORTRST $FD）でCPUを停止する。iocs.cの白帯フックから呼ぶ。
 * ブレークポイント等と同じ「デバッガの停止」として扱うため、DBG_STATUSは
 * STATE=paused STOP=WHITEBAND を返し、DBG_CONT で再開できる（停止理由ごとに
 * 別々の停止フラグを持つと、statusが実態と食い違い再開手段も分かれるため）。 */
void dbg_halt_whiteband(void);
void dbg_enter_snapshot_stop(void);
void dbg_wp_rearm_all(void);

/* DBG_STEP: n命令を同期実行し（x68k_cycle_catchup()で下流も同期）、
 * 実行後はpaused状態になる。戻り値は消費した公称サイクル数。 */
int dbg_step(x68k_bus_t *bus, int n);

/*
 * DBG_STATUS/DBG_WAIT/DBG_STEP/DBG_BREAK共通の応答本体を書き込む
 * （"OK "接頭辞・末尾改行は呼び出し側=commands.cが付与する）。
 * 戻り値はbufへ書き込んだ長さ（末尾NUL抜き、snprintfと同じ意味）。
 * 切り詰めが発生した場合（stopinfo全体がbufszに収まらない場合）、戻り値は
 * 実際に書き込めた長さではなく「本来必要だった長さ」になる（snprintfの
 * 戻り値をそのまま使っているため）。呼び出し側は戻り値がbufsz以上なら
 * 切り詰まったと判断すること（mcp_build_stopinfo_reply()参照）。
 */
int dbg_format_stopinfo(char *buf, size_t bufsz);

/*
 * stopinfoが収まる想定最大バイト数（NUL抜き）。内訳（実測・理論値、
 * 停止種別・レジスタ書式に基づく）:
 *   STATE=paused              13
 *   STOP=<kind> 最長          16  (BREAKPOINT/WATCHPOINT)
 *   MODE=AUTO                 10
 *   CAP=PC,REG                11
 *   EXEC=NORMAL               12
 *   WB=OFF                     7
 *   停止種別ごとの追加部最長  57  (WATCHPOINTでid=-2147483648・WPADDR=FFFFFFFF等)
 *   レジスタ部（固定長）     237
 *   ------------------------------
 *   理論最悪                 363
 *   余白（将来のフィールド追加分）149
 *   ==============================
 *   MCP_STOPINFO_MAX          512
 */
#define MCP_STOPINFO_MAX 512

/*
 * ブレークポイント管理。
 * mcp/commands.cの BP グループのハンドラから使う。
 *
 * dbg_bp_add(): cond_textが非NULL・非空文字列ならdbg_cond_parse()でパースし
 *   条件付きブレークポイントとして登録する。
 *   戻り値: 0以上=登録成功時のブレークポイントID（配列インデックスそのもの、
 *   MC68K_MAX_BREAKPOINTSと同じ範囲）。負値はエラー:
 *     -1 = 登録数上限（コア側 mc68k_add_breakpoint() の "full" 相当）、またはCPU未初期化
 *     -2 = cond_textの構文エラー
 *     -3 = addrが奇数
 */
int dbg_bp_add(uint32_t addr, const char *cond_text);
void dbg_bp_del(int id);
void dbg_bp_set_enable(int id, int enable);
void dbg_bp_clear(void);

/* BP_LIST応答本体（"id=0,addr=...,en=...,hits=...,cond=...;id=1,..."形式）を
 * bufへ書き込む。戻り値は書き込んだ長さ、*out_countは登録中のBP数。 */
int dbg_bp_format_list(char *buf, size_t bufsz, int *out_count);

/*
 * ウォッチポイント管理。
 * mcp/commands.cの WP グループのハンドラから使う。値変化のエッジ検出のみ
 * （書き込みアクセスそのものの検出ではない）で、mc68k_debug_run()経由の
 * 実行でのみ機能する（コア側debug.hの制約）。
 *
 * dbg_wp_add(): sizeは1/2/4のみ有効。
 *   戻り値: 0以上=登録成功時のウォッチポイントID。負値はエラー:
 *     -1 = 登録数上限、またはコア側failure（奇数addr等、区別できない）
 *     -2 = sizeが1/2/4以外
 */
int dbg_wp_add(uint32_t addr, int size);
void dbg_wp_del(int id);
void dbg_wp_set_enable(int id, int enable);
void dbg_wp_clear(void);

/* WP_LIST応答本体（"id=0,addr=...,size=...,en=...,hits=...,last=...;..."形式）
 * をbufへ書き込む。戻り値は書き込んだ長さ、*out_countは登録中のWP数。 */
int dbg_wp_format_list(char *buf, size_t bufsz, int *out_count);

/*
 * 例外ブレークマスク。
 * mc68k_set/get_exception_break_mask()への直接パススルー（ホスト側に状態を
 * 複製しない。dbg_needs_debug_run()もコアへ都度問い合わせる）。
 */
void dbg_set_exc_mask(uint64_t mask);
uint64_t dbg_get_exc_mask(void);

/*
 * ログハンドラ・ダンプ系。
 * mc68k_set_log_handler()を常時登録し、ダンプコマンド実行中だけキャプチャして
 * 生バイナリ応答で返せるようにする。main.cがCPU作成直後に一度だけdbg_init_log_handler()
 * を呼ぶ。
 */
void dbg_init_log_handler(void);

/*
 * dbg_dump_call_ring/pc_ring/reg_ring/fault_ring(): mc68k_dump_*_ring()の出力を
 * キャプチャしてbuf（NUL終端）へ書き込む。戻り値は書き込んだ長さ（NUL抜き）、
 * 失敗時（cpu未初期化・buf不正）は-1。countの意味はコア側APIに準拠（0=全件）。
 */
int dbg_dump_call_ring(char *buf, size_t bufsz, int count);
int dbg_dump_pc_ring(char *buf, size_t bufsz, int count);
int dbg_dump_reg_ring(char *buf, size_t bufsz, int count);
int dbg_dump_fault_ring(char *buf, size_t bufsz, int count);

/* BADJUMP/BADMOVEA/BADADDR/BADORI診断ログのON/OFF（対応するmc68k_set_*_log()
 * へのパススルー。cpu未初期化時は無視）。 */
void dbg_set_badjump_log(int on);
void dbg_set_badmovea_log(int on);
void dbg_set_badaddr_log(int on);
void dbg_set_badori_log(int on);
