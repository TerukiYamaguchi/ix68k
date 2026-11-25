---
exe: TX.R
種別: freeware
作者: はらし (M.Harashino)
版: v1.01 (1995-02-19)
出典: 同梱マニュアル tx.1（man形式）。配布元 http://retropc.net/x68000/software/tools/archiver/tx/
判別: -（配布アーカイブ TXR101MS.LZH 収録版のみ確認。v1.00はソース付きTXR100FS.LZHで別配布）
検証状態: ❌未着手（HDS導入のみ）
最終更新: 2026-08-19
---
# TX.R (tx)

> tar/gzip/tgzアーカイブ展開プログラム。拡張子`.tar`/`.gz`/`.tgz`のファイルを展開します。Human68k ver.2.02以降で動作。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式:

```
tx [-[xetc[012]]lvamniw] [-b path] tarfile [extfile [...]]
gx gzfile
```

`tarfile`は展開するアーカイブ名（拡張子省略時は`.tar`を付加、ワイルドカード可）。`extfile`は
展開したいファイル名（省略時は全ファイル、ワイルドカード可）。実行ファイル名の先頭を`g`に
した名前（`gx.r`等）でリンクを張ると、gzipファイルをデコードして標準出力へ流す動作になる
（本HDSには`tx.r`のみ導入、`gx`名でのリンクは未設定）。

展開の基本動作を決めるオプション:

| オプション | 効果 |
|-----------|------|
| `-x[012]` | 展開する（ディレクトリ構造を再現する）。指定なしのときの既定動作。数値でCRCテストのレベルを指定できる |
| `-e[012]` | 展開する（ディレクトリ構造は再現しない） |
| `-t[012]` | 展開はせずCRCテストだけ行う（`0`=全ファイル対象／`1`=行わない／`2`=行わない、既定`0`） |
| `-b path` | 展開先ディレクトリを指定する（アーカイブ側が絶対パスを指定していても強制的にここへ展開する） |
| `-z` | gzip圧縮ファイル（拡張子`.gz`）を展開する |
| `-p` | 展開結果を標準出力へ流す（v1.01の新機能。ChangeLog.101/tx.hedには記載があるがtx.1本文には未反映） |

表示・確認まわりのオプション:

| オプション | 効果 |
|-----------|------|
| `-l`, `-v` | アーカイブ内のファイル一覧を表示する |
| `-c[012]` | 同名ファイルが既にある場合の動作（`0`=常に上書き／`1`=展開側が新しい時だけ上書き／`2`=常に確認、既定`2`） |
| `-m` | 同名かつタイムスタンプが新しいファイルは確認なしでスキップする |
| `-n[012]` | 画面表示の制御（`0`=表示なし＋`-m`相当／`1`=標準出力／`2`=標準エラー出力、既定`0`） |

その他の動作オプション:

| オプション | 効果 |
|-----------|------|
| `-a` | 属性情報を復元する |
| `-i` | 展開ファイル名の指定で大文字小文字を区別する |
| `-w` | 複数ファイル展開時、いったんメモリ上にまとめてから一括でディスクへ書き出す |

終了コード: ヘルプ表示時・エラー時は非0を返す（ただしファイル書き出し自体の失敗は
エラー扱いにならない点に注意、と原資料に明記されている）。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/tools/archiver/tx/ から`TXR101MS.LZH`としてダウンロード可能（v1.01 限定公開版、ソースなし）。
**`arc/TXR101MS.LZH`にローカル保存済み**（2026-08-12取得）。再検証・再インストール時は
retropc.netへ再度アクセスせず、まずこちらを使うこと。

### インストール（SYSTEM_HD.HDSへの導入手順）

CONFIG.SYS登録不要。PATHの通ったディレクトリ（`\BIN`等）に`tx.r`本体を置くだけで`tx`コマンドとして使えます。

```sh
# 1. ダウンロード（arc/TXR101MS.LZHにローカル保存済みならそれを使い、再ダウンロードしない）
cp arc/TXR101MS.LZH ./TXR101MS.LZH
# 無ければ: curl -LO http://retropc.net/x68000/software/tools/archiver/tx/TXR101MS.LZH

# 2. 展開（unar、要 brew install unar。LZH形式）
unar -o ./tx_extracted TXR101MS.LZH
# → ./tx_extracted/TXR101MS/{ChangeLog.101, tx.hed, tx.1, tx.r}

# 3. 本体(tx.r)をHDSの\BINへコピー（hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./tx_extracted/TXR101MS/tx.r /BIN

# 4. ドキュメント(tx.1/tx.hed/ChangeLog.101)はHDSの\DOC\TXへコピー（ix68k上で参照用）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/TX
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./tx_extracted/TXR101MS/tx.1 /DOC/TX
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./tx_extracted/TXR101MS/tx.hed /DOC/TX
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./tx_extracted/TXR101MS/ChangeLog.101 /DOC/TX
```

`\DOC\TX`配下（2026-08-12導入）:`tx.1`（man形式マニュアル）・`tx.hed`（ヘッダ情報）・
`ChangeLog.101`（改変履歴）。

### MCP アクション

```
run_command("TX -l ARCHIVE.TAR")           # アーカイブ内一覧表示
run_command("TX ARCHIVE.TAR")              # カレントディレクトリへ展開
run_command("TX -e ARCHIVE.TGZ")           # ディレクトリ構造を無視して展開
run_command("TX -z ARCHIVE.GZ")            # gzip単体ファイルの展開
```

`\BIN`がPATH外の場合はフルパス指定（`C:\BIN\TX ...`）で呼び出します。

### 検証メモ

- 2026-08-12: `arc/TXR101MS.LZH`から`tx.r`をHDS(`\BIN`)へ導入済み。ix68kでの起動・展開動作は未検証（このセッションでは導入のみ実施）。
- 出典アーカイブ: `TXR101MS.LZH`（7.9KB、`ChangeLog.101`/`tx.hed`/`tx.1`/`tx.r`を同梱、v1.01・ソースなし）。
- 未検証: 全オプション（`-x`/`-e`/`-t`/`-l`/`-v`/`-a`/`-b`/`-c`/`-m`/`-n`/`-i`/`-w`/`-z`/`-p`）。
