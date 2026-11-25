# tools-host/

ホストOS（macOS / Linux / Windows）上で動かす補助ツールです。リリースZIPの `tools-host/` と
同じ名前・同じ構成で、ツールごとにディレクトリを分けています。Human68k上で動かすものは
[tools-x68k/](../tools-x68k/README.TXT) にあります。

## バージョン

`aiq-bridge` は 0.9、そのほか（`mcp/`、`ix68k-hostfile`、`cgrom_gen`、`hu68disk`、`sram_switch`）は 1.0 です。
各コマンドは `--version` で表示します（`hu68disk` と `sram_switch` は `-V` も可）。外部デバッガ `debugger/` も 1.0 です。

## 配布物に入るもの

| ディレクトリ | 用途 |
|---|---|
| `mcp/` | MCPサーバーとそのPython/uv依存定義。設定手順は `docs/x68k-usage/mcp/mcp_llm_setup.md` を参照。 |
| `ix68k-hostfile/` | ホストとHuman68k間で単一ファイルを転送するCLI。ZIPにはPyInstallerで固めた実行ファイルを入れる。 |
| `cgrom_gen/` | フォントから互換CGROMを生成する。 |
| `aiq-bridge/` | AIQ.X のホスト側ブリッジ（LLMでコマンド列を作る）。Pythonのスクリプトのまま入れる。macOS / Linuxのみ。 |
| `hu68disk/` | ディスクイメージ（XDF/HDS）の操作ツール。ZIPにはビルド済みバイナリを入れる。macOS / Linuxのみ。 |
| `sram_switch/` | `sram.dat` のメモリスイッチ設定（オフライン）。ZIPにはビルド済みバイナリを入れる。macOS / Linuxのみ。 |

## 配布物に入らないもの

| ディレクトリ | 用途 |
|---|---|
| `debugger/` | 外部デバッガ ix68k-debugger。 |
| `pgo/` | リリースビルド用のPGOプロファイル採取。 |
| `netbsd/` | NetBSD/x68kの `.hds` 内のFFSから1ファイルを取り出す。 |
| `dev/` | エミュレータの実装・回帰検証・開発用の補助（起動スクリプト、構文チェック、MCPツールの整合チェック、SBOM生成、FLOATドライバ試験用ディスク作成など）。個人環境に依存する起動・調査ツールは `local/tools/` に置く。 |
