# ix68k-hostfile

バージョン 1.0（`ix68k-hostfile --version` で表示）。

稼働中のix68kとホストの間で、ファイルを1つずつやり取りするCLIです。Python 3（標準ライブラリのみ）で
書かれたスクリプトで、同じZIPの `tools-host/mcp/ix68k_wire.py` を読みます（`mcp/` を移動しないこと）。
Human68k側の相手は `tools-x68k/hostfile/hostfile.x`（配布ZIPでは `tools-x68k/hostfile/HOSTFILE.X`）です。

```sh
python3 ix68k-hostfile put <ホストのファイル>   # ホストからHuman68kへ送る（Human68k側で HOSTFILE GET）
python3 ix68k-hostfile get <保存先のパス>       # Human68kがHOSTFILE PUTしたファイルを受け取る
python3 ix68k-hostfile status                   # 転送待ちの状態を表示
```

- macOS / Linux: AF_UNIXソケットで接続します。複数のix68kを起動している場合は、環境変数
  `IX68K_MCP_SOCK`または`--sock`で接続先を指定します。
- Windows: `127.0.0.1`のTCPで接続します（既定ポート26800）。`python` で実行します。
  `IX68K_MCP_PORT`または`--port`で変更できます。
- WSL上のCLIから、ネイティブWindows版のix68kへは接続できません。

詳しくは `../../docs/x68k-usage/mcp/daily_workflows.md` と
`../../docs/x68k-usage/guide/file_transfer.md` を参照してください。
