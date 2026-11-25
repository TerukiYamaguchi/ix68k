# 日常的な作業の流れ

MCP対応のAIエージェントがix68kを操作する際の標準フロー。人間が画面とキーボードで操作する場合は
[`guide/`](../guide/README.md) を参照します。

## コマンドを実行する

1. `disk(status)` または画面で起動媒体を確認します。
2. `key_wait(action="wait")` でプロンプト待ちを確認します。
3. `run_command("dir")` のように短い非破壊コマンドから実行します。
4. 対話型ソフトは個別リファレンスの「MCPアクション」またはキー操作に従います。

`paste_text` は文字を送るだけで完了を待ちません。通常のコマンド実行には `run_command` を使います。

## ファイルを転送する

MCPのHOSTFILEは、ホスト→ゲストなら `hostfile(action="put", macos_path="...")` の後にゲストで
`HOSTFILE GET NAME`、逆方向ならゲストで `HOSTFILE PUT NAME` の後に
`hostfile(action="get", macos_path="...")` を使います。正確な引数は `tools-host/mcp/MCP_TOOLS.md` を正本とします。

## 媒体を交換する

フロッピーは先に `disk(action="eject", media="fd", index=0)` を行ってから挿入します。固定SCSI HDDは
実行中の交換を前提にしません。変更前にバックアップを取り、必要ならリセットします。

## ソフトを導入する

親READMEの「ソフトウェアバンドルインストール」または対象ソフトの「インストール」節を使います。
`CONFIG.SYS` や `AUTOEXEC.BAT` を編集する場合は、元ファイルを別名で保存してから変更します。
