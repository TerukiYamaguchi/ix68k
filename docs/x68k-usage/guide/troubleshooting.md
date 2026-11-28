# トラブルシュート

| 症状 | 最初に確認すること |
| --- | --- |
| 起動しない | `ix68k.cfg` のROM/CGROM/媒体パス、`-rom`/`-cgrom`指定 |
| プロンプトが出ない | 起動媒体、`-boot fdd`/`-boot scsi`、[boot.md](https://github.com/TerukiYamaguchi/ix68k/blob/main/docs/ix68k-internals/boot.md) |
| ファイルが見えない | ホストとゲストのパス、HOSTDRVまたはディスクイメージ、現在ドライブ |
| 媒体交換後も内容が古い | フロッピーは排出してから挿入、固定HDDはリセットまたは再起動 |
| 文字入力が崩れる | `keyboard_layout`、ホスト側のキー配列、入力欄にフォーカスがあるか |

ROM・CGROM・`series` の指定ミスで起動しない場合は [boot_errors.md](boot_errors.md) を参照します。
詳細な起動・媒体設定は [boot.md](https://github.com/TerukiYamaguchi/ix68k/blob/main/docs/ix68k-internals/boot.md) を参照します。MCP接続の問題は
[mcp/](../mcp/README.md) を参照します。
