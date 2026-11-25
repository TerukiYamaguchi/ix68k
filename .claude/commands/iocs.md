# IOCS トラップハンドラ

`TRAP #15` をソフトウェアでインターセプトする（ROM ベクタに飛ばない）。
`mc68k_set_trap_handler()` 経由で登録（`main.c`）。

> **必須**: IOCS コールを実装・変更したら **`docs/develop/support/iocs_support.md`** を必ず更新する
> （全 IOCS $00–$FF のサポート状況一覧）。

## 実装状態

| D0 (fn) | 機能        | 実装状態                                              |
|---------|------------|-------------------------------------------------------|
| 0x00    | B_KEYINP   | ✅ ブロッキング待ち（MCP/SDL2 キューから dequeue）      |
| 0x01    | B_KEYSNS   | ✅ ノンブロッキング peek（キューを消費しない）           |
| 0x02    | B_SFTSNS   | ✅ SDL2 modifier state を返す                         |
| 0x20    | B_PUTC     | ✅ Shift-JIS 1文字を TVRAM に描画                     |
| 0x21    | B_PRINT    | ✅ Shift-JIS 文字列を TVRAM に描画                    |
| 0x34    | 出力レディ  | 常に 1（ready）                                       |
| 0x35    | 1文字出力   | ✅ TVRAM に ANK フォント描画                           |
| 0x43    | B_DSKINI   | FDC: 0、SCSI/未接続: -1                              |
| 0x44    | B_DRVSNS   | FDC: 2HD=0x03、未挿入=0、SCSI: -1（bit29=1）          |
| 0x46    | B_READ     | ✅ XDF セクタ読み込み（DMA 転送先アドレス使用）         |
| 0x4A    | B_ASSIGN   | スタブ (D0=0)                                        |
| 0x4E    | B_DRVCHK   | FDC: 2HD=2、未挿入=0                                 |
| 0x8E    | BOOTINF    | 0x90（FD0 からパワーオン起動）                        |
| others  | 未実装      | 成功扱い（D0=0）で返す                                |
