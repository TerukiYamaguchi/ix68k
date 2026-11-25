---
exe: TwentyOne.x
種別: freeware
作者: Ｅｘｔ(T.Kawamoto) さん（原作）／GORRY さん（TW136Cn拡張）
版: v1.36c+14（`tw136c14.lzh`収録、9978 bytes）
出典: 配布元 http://retropc.net/x68000/software/disk/filename/twentyone/ （X68000 LIBRARY）／
  同梱ソース内`TwentyOne.doc`・`readme.doc`／`VTwentyOne.doc`（TW136Cn改造履歴）
判別: -（配布アーカイブ`tw136c14.lzh`収録版のみ確認。VFAT対応版`VTwentyOne.x`は同梱だが未導入）
検証状態: ✅ SYSTEM_HD.HDSへ導入・CONFIG.SYS常駐・21文字ファイル名の識別をix68k確認済み。
  `DEL`ワイルドカード一括削除も含め異常なし（検証メモ参照）
最終更新: 2026-08-24
---
# TwentyOne.x

> メモリ上のHUMAN.SYSにパッチをあて、Human68kのファイル名同一性判定を（既定の）先頭8文字＋
> 拡張子3文字から、主ファイル名18文字＋拡張子3文字（計21文字）まで拡張する常駐ドライバ。
> 大文字小文字の区別・特殊文字の使用・複数ピリオドの使用も選択的に有効化できます。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

デバイスドライバとして`CONFIG.SYS`に、または常駐プログラムとしてコマンドラインから、の
どちらでも起動できます。`-B`を除くスイッチは常駐済みの状態からコマンドラインで再指定できます。

書式:

```
TwentyOne.x [オプション]
```

| オプション | 説明 | 既定 |
|-----------|------|------|
| `-B=<num>` | 拡張FILBUF（EXBUF）の確保数。`FILES`/`NFILES`コンパチビリティ維持用 | 256 |
| `+T` / `-T` | ファイル名を21文字（`+T`）／11文字相当（`-T`）認識する | `+T` |
| `+C` / `-C` | 大文字小文字を区別する（`+C`）／しない（`-C`） | `-C` |
| `+W` / `-W` | `+C`時のケースミスマッチ発生を警告表示する | `-W` |
| `+P` / `-P` | ファイル名にピリオドを複数使用可能にする | `-P` |
| `+S` / `-S` | `-`や`<`等の特殊文字・スペースを使用可能にする | `-S` |
| `+D` / `-D` | プリンタエコー（^P）を無効化する（Human 3.02以降） | `-D` |
| `+R` / `-R` | `/`始まりのパスに環境変数`SYSROOT`を補完する（Human 3.01以降） | `-R` |
| `+r` / `-r` | `+R`とほぼ同じだが`¥`始まりのパスにも`SYSROOT`を補完する | - |
| `+V` / `-V` | バーボースモード | `-V` |
| `+O` / `-O` | 現在オンになっているオプションを1行で表示する（fish等のスクリプト連携用） | - |

`-B=<num>`はEXBUFが不足すると`FILES`コールが`-8`（no memory error）を返す・
ファイルが見つからないエラーが頻発する等の症状が出るので、その場合に増量して使う
（常駐時にのみ有効）。

`+T`で認識される21文字のうち、実際に別ファイルとして区別されるのは主ファイル名部分
（18文字まで）＋拡張子3文字で、`-T`指定時はHuman68kオリジナルの動作（先頭8+3文字判定）
に戻る。

`+C`を有効にすると`COMMAND.X`と`Command.X`のようなケースミスマッチでアプリケーションが
ファイルを見失うことがある（`+W`はこの原因究明のための一時的な診断オプション）。

副次的な仕様拡張として、パス区切り`/`・`¥`の連続は1つの区切りとみなされる
（例: `A:/tmp//temp` → `A:/tmp/temp`）。

### オプション設定例（`TwentyOne.doc`より）

```
TwentyOne +TSV -B=512          # 21文字・特殊文字を有効にして常駐
TwentyOne +C                   # 常駐済みの状態から大文字小文字区別を追加
TwentyOne -TCPSR +V            # 常駐解除相当（全スイッチをオフに戻す。真の常駐解除機能は無い）
```

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/disk/filename/twentyone/ から `tw136c14.lzh`（124.2KB）。
収録物: `TwentyOne.x` / `VTwentyOne.x`（VFAT拡張ファイル名参照対応版）/ `VTwentyOne.doc` /
`Source.LZH`（ソース一式、基本版の使用法ドキュメント`TwentyOne.doc`・`readme.doc`はここに同梱）。

**`arc/retropc.net/x68000/software/disk/filename/twentyone/tw136c14.lzh` にローカル保存済み**
（2026-08-24取得）。再インストール・再検証時はretropc.netへ再アクセスせずこちらを使います。

### インストール（SYSTEM_HD.HDSへの導入手順）

```sh
# 1. ダウンロード（arc/ に無い場合のみ）
curl -L --create-dirs \
  -o "arc/retropc.net/x68000/software/disk/filename/twentyone/tw136c14.lzh" \
  "http://retropc.net/x68000/software/disk/filename/twentyone/tw136c14.lzh"

# 2. 展開（unar、要 brew install unar）
unar -o ./twentyone_out \
  "arc/retropc.net/x68000/software/disk/filename/twentyone/tw136c14.lzh"
# → ./twentyone_out/TW136C14/{TwentyOne.x, VTwentyOne.x, VTwentyOne.doc, Source.LZH}
# 基本版の使用法ドキュメントは Source.LZH 内: unar -o ./source_out ./twentyone_out/TW136C14/Source.LZH

# 3. 本体をHDSの\SYSへコピー（CONFIG.SYSデバイスドライバとして登録するため\BINでなく\SYS）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./twentyone_out/TW136C14/TwentyOne.x /SYS

# 4. ドキュメントを\DOC\TWENTYONE\へ配置
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/TWENTYONE
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./source_out/Source/TwentyOne.doc /DOC/TWENTYONE

# 5. CONFIG.SYSへ登録（HOSTDRV.SYSの後に追記。オプションなし＝既定の+Tで21文字認識のみ有効）
tools-host/hu68disk/hu68disk extract disk/SYSTEM_HD.HDS /CONFIG.SYS ./CONFIG.SYS.tmp
# 末尾（EOFマーカー$1Aの直前）に "DEVICE    = \SYS\TwentyOne.x\r\n" をCRLFで追記してから書き戻す
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./CONFIG.SYS.tmp /CONFIG.SYS
```

オプションなし（デフォルト`+T`のみ）で導入しています。`+C`（大文字小文字区別）や`+P`
（複数ピリオド）は既存ツール・スクリプトとの互換性リスクがあるため意図的に付けていません。

### 検証メモ

- 2026-08-24: `tw136c14.lzh`を`arc/`へ取得、`TwentyOne.x`を`\SYS`へ、ドキュメントを
  `\DOC\TWENTYONE\`へ配置。`CONFIG.SYS`にオプションなしで`DEVICE=`登録（CRLF+EOFマーカー
  `$1A`維持を確認）。
- 2026-08-24: エミュレータ起動でCONFIG.SYS処理中のバナー
  "TwentyOne (とぅぇにぃわん) Ver 1.36c(for Human Ver 3.xx)" 表示を確認、正常にロードされました。
- 2026-08-24: **主目的（8+3文字同一性判定の解除）をix68k確認**。`C:\TWTEST`に
  `AAAAAAAABBBBBBBB.TXT`と`AAAAAAAACCCCCCCC.TXT`（先頭8文字が同一）をCOPYで作成し、
  `DIR`で**2つの別ファイルとして識別・保持される**ことを確認（パッチ無しなら
  Human68kマニュアルのMASTERFL1/MASTERFL2の例同様に衝突するはず）。18文字の主ファイル名
  （`ABCDEFGHIJKLMNOPQR.TXT`）も全文字省略なく`DIR`表示されることを確認。
- 2026-08-24: 当初`DEL *.TXT`（ワイルドカード一括削除）でHuman68kのシステムエラー
  「ディスクの管理領域が破壊されています」が発生し不具合を疑ったが、
  再検証の結果**TwentyOne.x自体の問題ではないと判明**。直前に`COPY CON`でファイルを
  開いたまま（Ctrl+Zで正常終了させずに）ソフトリセットしていたことが原因だった
  （当時のMCPの`PASTE`機構がCtrl+Zを送れなかったため）。完全な新規リブート後、
  COPY CONを使わずファイル間COPYのみで同条件の3ファイルを作り`DEL *.TXT`を実行した
  ところ、エラーは再現せず正常に一括削除できた。
  **教訓（TwentyOne固有ではない）**: エミュレータをリセットする前はゲスト側で開いている
  ファイルを正常終了させておくこと。
- 未検証: `+C`（大文字小文字区別）・`+P`（複数ピリオド）・`+S`（特殊文字）等の非既定オプション、
  `VTwentyOne.x`（VFAT対応版）。
