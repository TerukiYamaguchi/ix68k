# ホストとゲストのファイル転送

ホストの `/path/file` とゲストの `C:\FILE` は直接共有されません。用途で方法を選びます。

| 方法 | 向く用途 | 注意 |
| --- | --- | --- |
| HOSTDRV | 作業ディレクトリを継続共有 | ゲストに `HOSTDRV.SYS` の導入が必要 |
| HOSTFILE | 単一ファイルの受け渡し | ゲストに `HOSTFILE.X`、ホストに `ix68k-hostfile`（またはMCPの `hostfile`）が必要 |
| ディスクイメージ | 初期導入・配布物 | 実行中の固定HDDへホストから直接書き込まない |

HOSTDRVは `ix68k.cfg` の `hostdrv_dir` でホスト側ディレクトリを指定し、Human68kのリモート
ドライブとして使います。日常的なソース編集・ビルドにはこれを推奨します。詳細は
[HOSTDRV ユーザーズマニュアル](hostdrv.md) と
[HOSTDRVを使うネイティブ開発](../development/native_build.md) を参照。

単一ファイル転送（HOSTFILE）は、ホストの `python3 tools-host/ix68k-hostfile/ix68k-hostfile`（`put` / `get` / `status`）と
ゲストの `HOSTFILE.X`（`tools-x68k/hostfile/`）を組み合わせます。MCPからの使い方は
[mcp/daily_workflows.md](../mcp/daily_workflows.md) を参照します。

## HOSTDRVと他のデバイスドライバの併用

`CONFIG.SYS` で `SUSIE.X` 等のドライバがドライブレターを `A:`〜`Z:` で直接指定している場合
（例: `DEVICE = \SYS\SUSIE.X -IDn R:`）、`A:` から連続しない“穴”のあるドライブレター構成に
なります。この構成では、**HOSTDRV.SYS に限らず、それ以降に登録される全てのデバイスドライバで
Human68k本体がアドレスエラーを起こして起動できなくなります**（Human68k 3.02本体側の制約）。

ドライブレターは自動割当（`*:` または `@:`）を使ってください。
