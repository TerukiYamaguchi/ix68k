#pragma once

/*
 * 外部デバッガ ix68k-debugger（tools-host/debugger/）をメニューから起動する。
 * 開発ツリー向けの機能で、起動スクリプトが見つからない環境（配布物など）では
 * 利用不可として扱う。実装は POSIX（macOS/Linux）が debugger_launcher.c、
 * Windows が debugger_launcher_win.c（同じ契約）。
 *
 * 起動スクリプトの場所は、環境変数 IX68K_DEBUGGER_LAUNCHER があればそれ、
 * 無ければカレントディレクトリ基準の tools-host/debugger/launch_debugger.sh
 * （Windows は tools-host\debugger\launch_debugger.cmd）。
 * ビルド機のパスを実行ファイルへ埋め込まないため、リポジトリルートで起動する
 * （起動用スクリプトが cd する）か、環境変数で指定する。
 * ix68k の MCP ソケット（IX68K_MCP_SOCK）は環境変数経由で子プロセスへ引き継がれる。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAL_DEBUGGER_LAUNCHED = 0,
    PAL_DEBUGGER_ALREADY_RUNNING, /* 前回起動したデバッガがまだ動いている */
    PAL_DEBUGGER_UNAVAILABLE,     /* 起動スクリプトが無い */
    PAL_DEBUGGER_FAILED,          /* fork に失敗した */
} pal_debugger_launch_result_t;

/* 起動スクリプトが読めるなら1、そうでなければ0。メニュー項目の有効判定に使う。 */
int pal_debugger_available(void);

/*
 * デバッガを別プロセスで起動する。待たずに戻る。二重起動はしない。
 * 失敗理由は pal_log() にも出す。
 */
pal_debugger_launch_result_t pal_debugger_launch(void);

#ifdef __cplusplus
}
#endif
