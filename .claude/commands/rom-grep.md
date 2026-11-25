# ROM 逆アセンブル調査

**逆アセンブルは毎回スクリプトで生成しない。事前生成済みファイルを grep する。**

ROM・SCSI・Human68k の逆アセンブル済みソースは
`<DIS_DIR>/disasm/` にある
（`iplromxv/`・`scsi16in/`・`human68k/`・`commandx/`・`format/` 等、`.S`/`.lab`）。
以下は IPLROMXV（XVI ROM）を例にした詳細手順。他ディレクトリも同じ流儀（`-a`必須・
アドレス小文字・絶対パス）でgrepする。

## ファイルパス

- 逆アセンブルリスト: `<DIS_DIR>/disasm/iplromxv/IPLROMXV.S`
- **アドレス付き逆アセンブル**（推奨・アドレス調査はこちら）: `<DIS_DIR>/disasm/iplromxv/IPLROMXV_addr.S`
  - 各行の右端にコメントで `;ff0430` 形式の 68k アドレスが付く。分岐先オペランド（`$00ff042c`）と行末アドレスの両方で grep できるため、行番号⇄アドレスの手計算が不要。
- ラベルファイル: `<DIS_DIR>/disasm/iplromxv/IPLROMXV.lab`

## grep の鉄則

- **`-a` 必須**。`.S` はデータ部に Shift-JIS を含みバイナリ判定されるため、`-a` なしでは silent に空振りする。
- **アドレスは小文字**。`.lab`/`.S` のアドレスは小文字6桁（例: `ff446e`）。大文字では当たらない。
- **絶対パス＋二重引用符**で指定する。
- **1呼び出し1 grep**（`;`/`&&` で連結しない、許可外コマンドへパイプしない）。
- **`grep -m N`** で出力を絞る（`| head` は使わない）。

## アドレスからコードを引く手順

```bash
# 1. アドレス → ラベル名（.lab から）
grep -anE 'ff446e' "<DIS_DIR>/disasm/iplromxv/IPLROMXV.lab"

# 2. ラベル名 → コード位置（.S から）
grep -anE '_MOD:' "<DIS_DIR>/disasm/iplromxv/IPLROMXV.S"

# 3. アドレスへの参照を探す（オペランドは $00ff449c 形式）
grep -anE '00ff449c' "<DIS_DIR>/disasm/iplromxv/IPLROMXV.S"
```

## ROM アドレス変換

IPLROMXV.DAT (128KB):
```
68k アドレス = 0xFE0000 + ファイルオフセット
例: ファイル 0x1066E → addr 0xFF066E
```

**注意**: このファイルは IPLROMXV.DAT (XVI ROM) のみ。IPLROM.DAT (初代) とは別物。
