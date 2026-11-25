# CLAUDE.md

@AGENTS.md

## Claude Code 固有

- 許可ルールは `Bash(grep:*)`・`Bash(rg:*)`（コロン書式。旧 `Bash(grep *)` は不可）。grep / rg は単独・絶対パス指定で呼ぶ（`cd &&`・パイプ・`echo`/変数代入での前置き禁止 — 先頭トークンがずれてマッチしない）。事前生成ファイルの単発確認は Read ツールを使う。
- スペースを含むパスの `\ ` エスケープは、ハーネスの安全チェック「Contains backslash-escaped whitespace」に許可リストとは無関係に却下される（確認ダイアログも出ない）。環境変数でパスを組み立てる場合も `"$VAR/..."` の形でダブルクォートする。
