# 転送・実行・確認

開発中の成果物はHOSTDRVで共有し、ゲスト側から実行します。1ファイルだけを扱う場合はHOSTFILEも使えます。
転送方法は [ガイド](../guide/file_transfer.md) を参照します。

通常の確認は `run_command`、画面確認はMCPの画面取得、異常時は `debug_control` と
`get_registers` を使います。ブレークポイントや逆アセンブルの詳細は `tools-host/mcp/MCP_TOOLS.md` を参照します。
