# MCPサーバ（Claude Codeなどからix68kを操作する）

バージョン 1.0。

ソースをビルドせずに、MCPサーバ機能でAIエージェント（Claude Code、Codex CLIなど）から
ix68kの画面・キーボード・ディスク・デバッガを操作するためのファイル一式です。

| ファイル | 内容 |
| --- | --- |
| `ix68k_mcp_server.py`、`ix68k_wire.py` | MCPサーバ本体（Python、stdio）。稼働中のix68kへ接続します |
| `pyproject.toml`、`uv.lock` | MCPサーバの依存パッケージ（`uv`が自動で導入します） |
| `MCP_TOOLS.md` | 提供するツールの一覧と引数のリファレンス |
| `text_profiles/` | ゲームなどIOCSを使わないソフトの文字出力を取得するプロファイル |
| `../../.mcp.json` | Claude Code用のMCPサーバ登録（ZIPの展開先に置いてあります） |

## 必要なもの

- [`uv`](https://docs.astral.sh/uv/)（Pythonと依存パッケージを自動で用意します）
  - macOS: `brew install uv`
  - Linux / macOS: `curl -LsSf https://astral.sh/uv/install.sh | sh`
  - Windows: `winget install astral-sh.uv`
- 展開したix68kと、ROM・ディスクの準備（展開先の`README.md`を参照）

## 使い方

1. ix68kを起動します。MCP用の通信口が自動で作られます。
   - macOS / Linux: `/tmp/ix68k_mcp.sock`（環境変数`IX68K_MCP_SOCK`で変更）
   - Windows: TCP `127.0.0.1:26800`（環境変数`IX68K_MCP_PORT`で変更）
2. ZIPの展開先ディレクトリで`claude`を起動します。展開先の`.mcp.json`が読み込まれ、
   MCPサーバ`x68k-emulator`の使用を確認されるので許可します。
   `/mcp`で接続状態を確認できます。
3. Claude Codeに、画面を見る、キーを送る、ディスクを操作する、などを頼みます。

別のディレクトリから使うときは、`.mcp.json`の`command`を`uv`の絶対パスに、
`args`のパスを展開先の絶対パスに書き換えて、そのディレクトリの`.mcp.json`として置いてください。
複数のix68kを同時に使う場合は、`env`に`IX68K_MCP_SOCK`（Windowsは`IX68K_MCP_PORT`）を足します。
詳細は`MCP_TOOLS.md`の「複数インスタンスの起動」節を参照してください。

> `MCP_TOOLS.md`などの記述にある`./build/ix68k`は、この配布物では展開先の`ix68k`
> （macOSは`ix68k.app/Contents/MacOS/ix68k`、Windowsは`ix68k.exe`）に読み替えてください。

## 資料

- `docs/x68k-usage/mcp/`: エージェントによる操作、接続設定、環境構築レシピ
- GitHub: <https://github.com/TerukiYamaguchi/ix68k>
