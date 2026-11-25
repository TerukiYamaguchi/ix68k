# ix68k-debugger

バージョン 1.0（`--version` で表示）。

`ix68k-debugger` は ix68k の MCP ソケットを使う外部デバッグモニタです。ix68k 本体を起動後、別のターミナルで起動します。

操作の詳細は [ユーザーズマニュアル](MANUAL.md) を参照してください。

```bash
python3 tools-host/debugger/ix68k_debugger.py
```

ix68k の `File > Open Debugger...` からも起動できます（macOS・Windows・Linux。開発ツリー向けで、リポジトリルートで ix68k を起動したときだけ有効）。メニューとターミナルのどちらからでも、`tools-host/debugger/launch_debugger.sh`（Windows は `launch_debugger.cmd`）を使えば tkinter のある Python を自動で選びます。詳しくは [ユーザーズマニュアル](MANUAL.md) の「1.2 起動」を参照してください。

macOS/Linux は `IX68K_MCP_SOCK`（既定 `/tmp/ix68k_mcp.sock`）、Windows は `IX68K_MCP_PORT`（既定 `26800`）を使います。明示指定は `--socket PATH` または `--port PORT` です。起動直後は接続せず、`Refresh ON` を押したときだけ状態を取得します。

Tk は pip 依存ではありませんが、Python 配布物にない場合があります。Homebrew Python は `brew install python-tk`、Debian/Ubuntu は `sudo apt install python3-tk` で導入してください。

`python3` が指す Python に tkinter がないと、`Tkinter が必要です` と表示して終了します。Homebrew では `python3` が最新版（例: 3.15）を指し、`python-tk@3.14` だけを導入していると、この状態になります。次のコマンドで tkinter の有無を確認し、tkinter のある Python をバージョン付きで指定して起動してください。

```bash
python3 -c 'import tkinter'           # エラーなら python3 には tkinter がない
python3.14 -c 'import tkinter'        # エラーなしなら python3.14 を使える
python3.14 tools-host/debugger/ix68k_debugger.py
```

`python3.14` は例です。導入した `python-tk@<バージョン>` に合わせて読み替えてください。

更新は短命接続です。1接続内で HELLO を検証して同期・読み取り専用要求をパイプライン送信後に切断するため、Claude Code やスクリプトと交互に使えます。別クライアントが `DBG WAIT` または `KEY WAIT` 中なら busy と表示して再試行します。GUI自身は待機コマンドを送信しません。

主な操作は F5 (Run)、Shift+F5 (Break)、F10 (Step)、F11 (Step ×10)、Ctrl+B (BP追加)、Delete (BP削除)、Ctrl+G (メモリ移動)、Ctrl+L (PCへ戻る) です。Delete / BackSpace は BP / WP 一覧をクリックしてフォーカスがあるときだけ効きます（アドレス入力欄の編集と衝突させないため）。macOS では F11 が「デスクトップを表示」に割り当てられているため、そのままでは届きません。Shift+F11 を使うか、システム設定のキーボードショートカットで割り当てを外してください。`MEM READ` は I/O に副作用を持つため、`Read once` を明示操作した場合だけ使用します。

`調査終了` は BP/WP/例外/診断設定をまとめて解除します。他クライアントにも影響するため、実行コマンドを表示して確認します。

## 動作確認状況

- macOS: 実機 ix68k 起動中の GUI 手動確認まで完了。
- Linux: 完了（Ubuntu 22.04/WSL2）。ヘッドレスビルド（`-DPLATFORM_NULL=1`）で `mcp_server_pipeline_test` を含む ctest 全件、`/tmp/ix68k_mcp.sock` 経由の wire クライアント一連操作（HELLO/pipeline/BP/Break/Step/ring capture/調査終了）、GUI の `Refresh ON` 実接続・表示更新を確認。vcpkg SDL3 の GUI版（ix68k本体）でも同じ wire 検証と外部デバッガ接続を確認したが、**WSLg既定のWaylandバックエンドではix68k本体のメインループ（描画とMCP tickが同一ループ）が数十秒単位で停止し、MCP要求がtimeoutする**。`SDL_VIDEODRIVER=x11` を指定するWSLg既知の回避策（[build.md](../../docs/ix68k-internals/build.md)のWayland節参照）で解消し、HELLOが即時応答するようになりました。ネイティブX11/Waylandでの再現性は未確認。システムの `python3`（3.10）は `tools-host/mcp/ix68k_mcp_server.py` が要求する `tomllib`（3.11+）を持たないため、同ファイルに依存する `tests/test_mcp_port_py.py` は未実行（GUI本体が使う `ix68k_wire.py`/`ix68k_debugger.py` はこの制約を受けない）。
- Windows: 完了。TCP ループバック `127.0.0.1:26800` 経由で wire クライアント一連操作と GUI の `Refresh ON` 実接続・表示更新を確認。`mcp_server_pipeline_test` はビルド未対象（`if(NOT WIN32)`、TCP経路は別実装のため設計どおり）。
