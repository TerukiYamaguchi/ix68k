---
exe: LX.R
種別: freeware
作者: はらし (M.Harashino)
版: v1.20 (1995-04-30)
出典: 同梱マニュアル lx.1（man形式）。配布元 http://retropc.net/x68000/software/tools/archiver/lx/
判別: -（配布アーカイブ LXR120FS.LZH 収録版のみ確認）
検証状態: ✅ 起動・一覧表示(-1)・展開(-mb)をix68k確認済み
最終更新: 2026-08-19
---
# LX.R (lx)

> LZHアーカイブ高速展開プログラム。拡張子`.LZH`のファイルを`-lh0-`/`-lh1-`/`-lh4-`/`-lh5-`形式で展開、自己展開形式にも対応。Human68k ver.2.02以降で動作。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式:

```
lx [-[xetc[012]]lv1amnip[sw]f] [-b path] lzhfile [extfile [...]]
```

`lzhfile`は展開元のアーカイブ（拡張子省略時は`.lzh`が補われる、ワイルドカード可）。
`extfile`は展開したい個別ファイル名（省略時は全ファイル、ワイルドカード可）。環境変数
`lx_opt`の内容がコマンドラインの先頭に自動的に挿入されます。

| オプション | 効果 | 検証 |
|-----------|------|----|
| `-x[012]` | 展開する（ディレクトリ構造を再現）。何も指定しない場合の既定動作。数値でCRCテストの厳密さを指定 | 📖 |
| `-e[012]` | 展開する（ディレクトリ構造は再現しない） | 📖 |
| `-t[012]` | 展開はせずCRCテストのみ行う（0=全ファイル対象/1=`-lh0-`形式のみ/2=テストなし、既定は0） | 📖 |
| `-l`, `-v` | アーカイブ内のファイル一覧を表示する | 📖 |
| `-1` | ファイル名のみを一覧表示する | ✅ `LX -1 LXR120FS.LZH`で5ファイル名を正しく表示 |
| `-a` | ファイルの属性情報を復元する | 📖 |
| `-b path` | 展開先ディレクトリを指定する（アーカイブ側に絶対パスの記録があっても、指定先へ強制的に展開する） | ✅ `-mb`で使用、正しく指定先へ展開 |
| `-c[012]` | 同名ファイルが既にある場合の扱い（0=常に上書き/1=展開側が新しい時のみ確認/2=常に確認、既定は0） | 📖 |
| `-m` | 同名ファイルがあっても確認なしでスキップする | ✅ `-mb`で使用、無人展開を確認 |
| `-n[012]` | 画面表示の出力先を制御する（0=表示なし＋`-m`相当/1=標準出力/2=標準エラー出力、無指定時の既定は標準エラー出力） | 📖 |
| `-i` | 展開ファイルの指定で大文字小文字を区別する | 📖 |
| `-p` | 標準出力へ展開する（`-n`を同時指定した扱いになる） | 📖 |
| `-s` | 展開先の空き容量を事前確認する（`-w`とは同時指定不可） | 📖 |
| `-w` | メモリ上にまとめて展開してから、まとめてディスクへ書き出す（`-s`とは同時指定不可） | 📖 |
| `-f` | 破損したアーカイブでも強制的に展開する | 📖 |

終了コードはヘルプ表示時・エラー時に非0になる（ただしファイル書き出し失敗自体は
エラー扱いにならないと原文に明記されている点に注意）。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/tools/archiver/lx/ から`LXR120FS.LZH`としてダウンロード可能（v1.20 正式公開版、ソースあり）。
**`arc/LXR120FS.LZH`にローカル保存済み**（2026-07-30取得）。再検証・再インストール時は
retropc.netへ再度アクセスせず、まずこちらを使うこと。

### インストール（SYSTEM_HD.HDSへの導入手順）

CONFIG.SYS登録不要。PATHの通ったディレクトリ（`\BIN`等）に`lx.r`本体を置くだけで`lx`コマンドとして使えます。

```sh
# 1. ダウンロード（arc/LXR120FS.LZHにローカル保存済みならそれを使い、再ダウンロードしない）
cp arc/LXR120FS.LZH ./LXR120FS.LZH
# 無ければ: curl -LO http://retropc.net/x68000/software/tools/archiver/lx/LXR120FS.LZH

# 2. 展開（unar、要 brew install unar。LZH形式）
unar -o ./lx_extracted LXR120FS.LZH
# → ./lx_extracted/LXR120FS/{ChangeLog.120, lx.hed, lx.1, lx.r, lx.has}

# 3. 本体(lx.r)をHDSの\BINへコピー（hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./lx_extracted/LXR120FS/lx.r /BIN

# 4. ドキュメント(lx.1/lx.hed/ChangeLog.120)はHDSの\DOC\LXへコピー（ix68k上で参照用）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/LX
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./lx_extracted/LXR120FS/lx.1 /DOC/LX
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./lx_extracted/LXR120FS/lx.hed /DOC/LX
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./lx_extracted/LXR120FS/ChangeLog.120 /DOC/LX
```

`\DOC\LX`配下（2026-08-12導入）:`lx.1`（man形式マニュアル）・`lx.hed`（ヘッダ情報）・
`ChangeLog.120`（改変履歴）。`lx.has`（ソース）は実行に不要のためHDSへはコピーしません。

### MCP アクション（実行例）

```
run_command("LX -l ARCHIVE.LZH")           # アーカイブ内一覧表示
run_command("LX ARCHIVE.LZH")              # カレントディレクトリへ展開
run_command("LX -e ARCHIVE.LZH")           # ディレクトリ構造を無視して展開
run_command("LX -1 ARCHIVE.LZH")           # ファイル名のみ一覧表示（✅検証済み）
run_command("LX -mb DESTDIR ARCHIVE.LZH")  # 確認なしでDESTDIRへ展開（✅検証済み）
```

`\BIN`がPATH外の場合はフルパス指定（`C:\BIN\LX ...`）で呼び出します。

### 検証メモ

- 検証: 2026-07-30、SYSTEM_HD.HDS(`\BIN\LX.R`)からix68k起動して確認。
  自身の配布アーカイブ`LXR120FS.LZH`をテスト入力に使用（`\WORK`へ一時配置、検証後削除）。
  - `LX -1 LXR120FS.LZH`: アーカイブ内5ファイル名（`ChangeLog.120`/`lx.hed`/`lx.1`/`lx.r`/`lx.has`）を正しく表示
  - `LX -mb OUT LXR120FS.LZH`: 実展開に成功、`-lh5-`形式・展開後5ファイルのサイズが元アーカイブ内容
    （`lx.1`収録のarchive一覧と一致）・展開先の実ファイルサイズもホスト側オリジナルと1バイトも違わず一致
- 出典アーカイブ: `LXR120FS.LZH`（24.0KB、`ChangeLog.120`/`lx.hed`/`lx.1`/`lx.r`/`lx.has`を同梱）。
- 未検証: `-x`（ディレクトリ構造再現）・`-t`（CRCテスト）・`-c`（同名ファイル確認動作）等その他オプション。
