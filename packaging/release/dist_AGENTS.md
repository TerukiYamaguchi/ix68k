# AGENTS.md

このディレクトリは、X68000 エミュレータ ix68k のビルド済み配布版（リリースZIPの展開先）です。
ソースコードと開発者向け資料は含みません。AIエージェント（Claude Code、Codex CLI など）は、
作業の前に以下の資料を読み、その手順に従ってください。

## 最初に読む資料

| 資料 | 内容 |
| --- | --- |
| `README.md` | 同梱物、ROM・ディスクイメージの配置、起動方法 |
| `docs/x68k-usage/manual.md` | 起動・終了・MCP操作・付属資料の入口 |
| `docs/x68k-usage/README.md` | **エージェント向け参照ルール**と、X68000 ソフトの逆引きインデックス |
| `docs/x68k-usage/mcp/README.md` | MCP による操作（接続設定、運用ガイド、日常の操作フロー、環境構築レシピ） |
| `tools-host/mcp/MCP_TOOLS.md` | MCP ツールの引数・応答形式（正本） |

## 守ること

- X68000 上のソフトを操作するよう頼まれたら、`docs/x68k-usage/README.md` の逆引きインデックスで
  該当する文書を探し、その「MCP アクション」に従って操作する。推測でキーを送らない。
- 環境構築（Human68k のHDDインストール、音源ドライバの導入など）は、
  `docs/x68k-usage/mcp/provisioning/` の手順に順番どおり従う。
- IPLROM・CGROM・Human68k のディスクイメージは同梱していない。入手と配置は `README.md` の
  「1. 導入する」に従う。
- 利用者のディスクイメージ（`disk/` など）を上書き・削除する前に、利用者に確認する。
- `tools-x68k/*/README.TXT` は Human68k 上で読むための Shift-JIS（CRLF）のテキスト。
  ホストで読むときは文字コードを変換する（例: `iconv -f CP932 -t UTF-8`）。
- 開発者向け資料（`docs/ix68k-internals/`）とソースは GitHub にある:
  <https://github.com/TerukiYamaguchi/ix68k>
- ix68k 自体の不具合と思われる場合は、再現手順をまとめ、GitHub の Issue で報告するよう利用者に提案する。
