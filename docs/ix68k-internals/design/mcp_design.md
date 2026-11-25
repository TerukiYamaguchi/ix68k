# MCP 設計

## 1. 位置付けと構成

MCP は ix68k の診断・自動操作インターフェースです。Python の MCP サーバと、ix68k 本体の
独自ワイヤプロトコルを接続します。公開ツールの完全な一覧と操作例は `tools-host/mcp/MCP_TOOLS.md` を正本とします。

| 層 | 主なファイル | 責務 |
|---|---|---|
| MCP サーバ | `tools-host/mcp/ix68k_mcp_server.py` | FastMCP ツール、引数検証、ワイヤ応答の変換 |
| 接続 | `src/mcp/server.c`、`sock_path.c`、`port.c` | Unix domain socket（Windows は TCP loopback）、単一クライアント処理 |
| コマンド | `src/mcp/commands.c` | ワイヤプロトコル、コマンド群、エラーコード |
| デバッグ | `src/mcp/debugger.c`、`dbg_cond.c` | 停止／step、ブレーク・ウォッチポイント、条件、診断リング |
| 表示・文字 | `screenshot.c`、`text_event.c`、`text_probe.c` | 画面、テキストイベント、テキスト抽出 |
| 性能計測 | `perf.c` | 仮想CPUサイクルの累積と区間ラッチ（`PERF`） |

## 2. 接続とプロトコル

- macOS/Linux の既定接続先は `/tmp/ix68k_mcp.sock`、Windows は `127.0.0.1:26800`。
  複数インスタンスは `IX68K_MCP_SOCK` または `IX68K_MCP_PORT` を双方へ指定します。
- ワイヤプロトコル版は C 側と Python 側で一致させます。接続時に版を照合し、不一致では操作しません。
- C 側は単一クライアントを同期処理します。長時間待機を必要とする操作は、実行再開と待機を分け、
  他の操作を不必要に塞ぎません。
- Windows は AF_UNIX の可用性ではなく ix68k 本体の TCP listener を基準に選択します。TCP の
  ポート値は ASCII 数字かつ 1024--65535 に限定します。
- エラーは `UNKNOWN`、`BADARG`、`NOTREADY`、`TIMEOUT`、`NOCPU`、`FULL`、`IO`、`OOM`、
  `INTERNAL` に正規化します。

## 3. デバッグと非侵襲観測

デバッグ実行は `AUTO`／`ON`／`OFF` を持ちます。`AUTO` はブレークポイントまたは
ウォッチポイント登録時だけデバッグ実行経路を使います。停止中も MCP、入力、ラスタ・描画は進め、
画面はフレーム合成フォールバックで完成させます。

- ブレークポイントはアドレス、条件、パスカウント、複数登録に対応します。
- ウォッチポイント、例外ブレーク、`PCRING`／`REGRING`／`CALLRING`、診断ログを提供します。
- メモリ観測は副作用を避けるため `peek` 経路を使います。通常の MMIO read は FDC FIFO 等を変化させ得ます。
- IOCS のブレーク位置は ROM 固定アドレスではなく、RAM の IOCS ベクタから現在の実体を解決します。
- テキストイベントは入口プローブとプロファイルを使い、CPU を停止せずに文字列・カーソル情報を
  取得します。プロファイルは `tools-host/mcp/text_profiles/` に置き、無効ポインタはエラーイベントにします。

## 4. ツール群

ツールは操作目的ごとに集約し、個別のワイヤコマンド数をそのまま MCP ツール数へ増やしません。

| 分類 | 主な内容 |
|---|---|
| 実行・診断 | `debug_control`、`breakpoint`、`exception_break`、`diag_log`、`dump_ring`、`disassemble` |
| 状態観測 | レジスタ、メモリ、画面、テキスト、キー待機 |
| 入力 | テキスト、キー、マウス、ジョイスティック |
| メディア・転送 | FD/SCSI 挿抜・WP、HOSTFILE |
| エミュレータ操作補助 | アプリ起動／終了、画面撮影、MCP テキストプロファイル |

各ツールの引数、戻り値、制約は `tools-host/mcp/MCP_TOOLS.md` を更新して管理します。

外部デバッガ GUI は未実装です。将来追加する場合も、既存 MCP と同じ短命接続・HELLO・
プロトコル版照合を共有し、停止待ちで単一クライアントを占有しません。

## 5. 検証と保守

- `debugger_test` と MCP 関連テストで停止、条件、非侵襲 read、履歴を確認します。
- `tools-host/dev/check_mcp_tools.py` は Python の `@mcp.tool()`、設定、`MCP_TOOLS.md` の追随を検査します。
- MCP ツール追加・改名時は、C/Python のプロトコル版、ツールカタログ、設定の整合を同時に確認します。

## 変更履歴

| 日付 | 内容 |
|---|---|
| 2026-09-11 | MCP デバッガ移行計画から現行の MCP 設計を統合 |
| 2026-09-11 | ソケット複数インスタンス、Windows TCP、テキストプローブ、外部デバッガ方針を統合 |
