# AGENTS.md

Claude Code / Codex 共用の必須ルール。詳細な運用手順は必要なときだけ
`docs/ix68k-internals/agent_workflow.md` を読む。`CLAUDE.md` は本ファイルを取り込む。

## 常に守ること

- 既存の未コミット変更はユーザーの作業として保護し、依頼と無関係な変更をしない。
- 破壊的操作、履歴改変、本番変更、`git push`（特に force push）は明示指示なしに行わない。
- `extern/cmc68000/` は submodule pin の更新以外では編集しない。コア本体の変更は `../cmc68000/` で行う。
- ix68k を「本エミュレータ」と呼ばない。「実機」は物理的な X68000 のみを指す。
- 実装したら関連するビルド・テストを実行し、警告・失敗・未検証事項を報告する。テストを削除・スキップして通してはならない。
- IOCS コールを変更したら `docs/ix68k-internals/support/iocs_support.md` を更新する。
- サブディレクトリに `AGENTS.md` / `CLAUDE.md` があれば、作業前に読む。

## 開始時と参照先

- Codex は開始時に `gh issue list` を実行する。cmc68000 更新時の手順は詳細文書を参照する。
- `/build` のようなスキル名は `.claude/commands/<名前>.md`。Codex では該当ファイルを直接読んで従う。
- デバッグ、ROM、X68000 ソフト操作、NetBSD、retropc.net、MCP、レビュー、引き継ぎ、公開コメント、コスト最適化は、着手前に詳細文書の該当節を読む。

## 詳細運用

- [エージェント作業手順](docs/ix68k-internals/agent_workflow.md)
- [MCP デバッグ手順](.claude/commands/mcp-debug.md)
- [X68000 ソフトウェア索引](docs/x68k-usage/README.md)
