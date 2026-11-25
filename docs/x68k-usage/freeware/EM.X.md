---
exe: EM.X
種別: freeware
作者: Daniel M. Lawrence（原作）/ lika・homy・SALT・peace・shuna・rima（X68k移植）/ Sharl Morlaroll（SharlEMACS拡張）
版: MicroEMACS 3.10 j1.43 (rel.5) + しゃれまっくす(SharlEMACS) rel.5c6
出典: http://retropc.net/x68000/software/texteditor/microemacs/microemacs/ （本体・マニュアル）、
      http://retropc.net/x68000/software/texteditor/microemacs/sharlemacs/ （SharlEMACS拡張版）
判別: SharlEMACS版em.x（205456バイト、"HU"ヘッダ）。オリジナルMicroEMACS版em.x（211150バイト）とは別バイナリで、
      本HDSにはSharlEMACS版のみ導入（下記「ix68kでの利用」参照）
検証状態: ✅ HDSインストール・PATH経由起動・終了(^X^C)をix68k確認
最終更新: 2026-08-27
---
# EM.X (MicroEMACS / SharlEMACS)

> EMACS系テキストエディタ。オリジナルはDaniel M. Lawrence作のMicroEMACSをlika氏らがX68000へ移植した版（3.10 j1.43 rel.5）。
> 本HDSにはさらにSharl Morlaroll氏が機能拡張した「しゃれまっくす(SharlEMACS) rel.5c6」を導入しています。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式:

```
EM [ファイル名...]
```

引数なしなら `*scratch*` バッファで起動します。

### 起動後の操作（対話型コマンドのみ）

キー手順はMicroEMACS標準（GNU Emacsに近いCtrl系バインド）に、`custom/keybind.el`によるファンクションキー拡張が加わる。

| 操作 | キー手順 | 検証 |
|------|---------|----|
| 終了（保存確認あり） | `^X^C` | ✅ |
| クイック終了（保存して即終了） | `M-e`（quick-exit） | 📖 |
| ファイルを開く | `^X^F` | 📖（MicroEMACS標準） |
| 保存 | `^X^S` | 📖（MicroEMACS標準） |
| dired（ファイラ）起動 | `F1` | 📖 |
| バッファ選択メニュー | `F2` | 📖 |
| 文字列検索 | `F4` | 📖 |
| シェル起動 | `S-F10` / `FNx` | 📖（`ESHELL`環境変数依存、下記参照） |
| キーバインド一覧 | `A-F6`（describe-bindings） | 📖 |

拡張ファンクションキー全体の一覧は `document/rc.man`（`\DOC\EMACS`へ導入済み）を参照。

---

## ix68kでの利用

### 入手先

- MicroEMACS本体: http://retropc.net/x68000/software/texteditor/microemacs/microemacs/
  （`em_r5b.lzh` バイナリ一式、`em_man.lzh` 日本語マニュアル）
- しゃれまっくす(SharlEMACS): http://retropc.net/x68000/software/texteditor/microemacs/sharlemacs/
  （`emr5c6b.tgz` バイナリ）

**`arc/retropc.net/x68000/software/texteditor/microemacs/`にローカル保存済み**
（`microemacs/em_man.lzh` `microemacs/em_r5b.lzh` `sharlemacs/emr5c6b.tgz` `sharlemacs/r5c6030.lzh`、
2026-08-27取得）。再検証・再インストール時はretropc.netへ再アクセスせず、まずこちらを使うこと。

### SharlEMACSの「差分」について（重要）

配布元ページの説明文には「しゃれまっくす(SharlEMACS)は MicroEMACS に対するバイナリ差分形式で配布されており、
更新に`bup.x`を使う」とあるが、実際に`emr5c6b.tgz`を展開して確認したところ、同梱の`em.x`は`bup.x`用の
`.bfd`差分ファイル（"BD"マジック）ではなく、**それ自体が完成したHuman68k実行ファイル**（"HU"マジック、
205456バイト）でした。`.bfd`形式の差分ファイルは同梱の`mines.bfd`（付属の縮小版ゲーム"mines"用、本体とは無関係）
のみで、`em.x`には存在しません。したがって本インストールでは`bup.x`は使わず、`emr5c6b.tgz`収録の`em.x`を
そのままMicroEMACS本体の`em.x`と差し替える形で導入した（配布元README「展開方法 tgz xf」の指示通り）。

X68030向けの追加バイナリ差分`r5c6030.lzh`（`.bfd`形式、`em030.bfd`）も配布されているが、今回は導入していない
（`disk/SYSTEM_HD.HDS`は68000/X68030共用のため。X68030専用構成で導入する場合は別途検討）。

### インストール（HDSへの導入手順）

MicroEMACSは単一実行ファイルではなく、`%dir-path`配下に`el/`（マクロ）・`custom/`（設定）・`complete/`
（補完辞書）・`etc/`（フォント等）・`help/`が揃って初めて動く構成のため、専用ディレクトリ`\EMACS`へ
一式まとめて導入します。

```sh
# 1. ダウンロード（arc/retropc.net/...にローカル保存済みならそれを使う）
BASE="arc/retropc.net/x68000/software/texteditor/microemacs"
# 無ければ:
# curl --insecure -L --create-dirs -o "$BASE/microemacs/em_r5b.lzh" \
#   "http://retropc.net/x68000/software/texteditor/microemacs/microemacs/em_r5b.lzh"
# curl --insecure -L --create-dirs -o "$BASE/microemacs/em_man.lzh" \
#   "http://retropc.net/x68000/software/texteditor/microemacs/microemacs/em_man.lzh"
# curl --insecure -L --create-dirs -o "$BASE/sharlemacs/emr5c6b.tgz" \
#   "http://retropc.net/x68000/software/texteditor/microemacs/sharlemacs/emr5c6b.tgz"

# 2. 展開（unar / tar。LZH形式にはunar、TGZにはtar）
unar -o ./em_man_extracted "$BASE/microemacs/em_man.lzh"
unar -o ./em_r5b_extracted "$BASE/microemacs/em_r5b.lzh"
tar xzf "$BASE/sharlemacs/emr5c6b.tgz" -C ./sharlemacs_extracted   # → ./sharlemacs_extracted/emr5c6/

# 3. em_plain.rc / em_dump.rc の %dir-path、emacs.rc のsourceパスを
#    "D:\EMACS\" → "C:\EMACS\" へ書き換える（install.doc記載の必須手順。
#    3ファイルとも該当行はASCII文字のみなのでバイト単位置換で安全）
#    perl -pi -e 's/D:\\EMACS\\/C:\\EMACS\\/g' em_plain.rc em_dump.rc emacs.rc

# 4. ディレクトリ作成（hu68disk、エミュレータ停止中に実行）
for d in /EMACS /EMACS/COMPLETE /EMACS/CUSTOM /EMACS/EL /EMACS/EL/DIRED /EMACS/EL/MENU \
         /EMACS/ETC /EMACS/HELP /DOC/EMACS /DOC/EMACS/SHARL; do
  tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS "$d"
done

# 5. ランタイム一式をコピー（em.xはSharlEMACS版に差し替え。他はMicroEMACS本体から）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./em_r5b_extracted/em_r5b/emacs.rc    /EMACS   # 編集済み
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./em_r5b_extracted/em_r5b/em_plain.rc /EMACS   # 編集済み
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./em_r5b_extracted/em_r5b/em_dump.rc  /EMACS   # 編集済み
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./sharlemacs_extracted/emr5c6/em.x    /EMACS   # SharlEMACS版
for f in ./em_r5b_extracted/em_r5b/complete/*; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /EMACS/COMPLETE
done
for f in ./em_r5b_extracted/em_r5b/custom/*.el; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /EMACS/CUSTOM
done
for f in ./em_r5b_extracted/em_r5b/el/*.el; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /EMACS/EL
done
# SharlEMACS追加マクロ（html.el/zcat.el）もel直下へ
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./sharlemacs_extracted/emr5c6/html.el /EMACS/EL
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./sharlemacs_extracted/emr5c6/zcat.el /EMACS/EL
for f in ./em_r5b_extracted/em_r5b/el/dired/*.el; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /EMACS/EL/DIRED
done
for f in ./em_r5b_extracted/em_r5b/el/menu/*; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /EMACS/EL/MENU
done
for f in ./em_r5b_extracted/em_r5b/etc/*; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /EMACS/ETC
done
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./em_r5b_extracted/em_r5b/help/emacs.hlp /EMACS/HELP

# 6. ドキュメント（MicroEMACS本体分 → \DOC\EMACS、SharlEMACS拡張分 → \DOC\EMACS\SHARL）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./em_r5b_extracted/em_r5b/readme.doc  /DOC/EMACS
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./em_r5b_extracted/em_r5b/install.doc /DOC/EMACS
for f in ./em_r5b_extracted/em_r5b/document/*; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /DOC/EMACS
done
for f in ./em_man_extracted/em_man/*; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS "$f" /DOC/EMACS
done
for f in README FEATURES CHANGES OCHANGES FAQ MINES; do
  tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./sharlemacs_extracted/emr5c6/$f /DOC/EMACS/SHARL
done
```

導入したファイルは `\EMACS`（本体173ファイル）・`\DOC\EMACS`（本体マニュアル24ファイル）・
`\DOC\EMACS\SHARL`（SharlEMACS拡張分ドキュメント6ファイル）に配置済み（2026-08-27時点）。

以下は`dumpkit/`（ダンプ版ビルドツール）・`idol.*`/`makedump.bat`/`makecomp.x`/`error.cmd`
（補完辞書・ダンプ生成用の開発者向けツール）で、通常の起動には不要なため導入していません。
X68030専用バイナリ差分（`r5c6030.lzh`）も同様に未導入。

### AUTOEXEC.BAT の追記（必須）

`install.doc`記載の必須環境変数（シェル起動`M-!`等でem.xが直接`getenv()`します。本体`em.x`の
strings出力で`ESHELL`/`ESHELL_OPT`/`ESHELL_QUOTE`/`SLASH`の各シンボルを確認済み）と、
`EM`コマンドをPATH経由で呼べるようにする追記を行った（2026-08-27、CRLF改行のまま追記）:

```
PATH C:\;C:\SYS;C:\BIN;C:\BASIC2;C:\ETC;C:\EMACS;
...
SET ESHELL=command.x
SET ESHELL_OPT=
SET ESHELL_QUOTE=
SET SLASH=\
```

（`ESHELL=command.x`は本HDSのデフォルトシェルがCOMMAND.Xであるため。`install.doc`はfish.x等の
un*x風シェル向け設定例も示しているが、本HDSでは使用していないので該当しません。）

`custom/envset.el`の`$dired-ls`（`ls.x -alDMU`）・`$trash`（`g:\temp\`）・`$dict-path`
（`a:\usr\load\lib`）はMicroEMACS配布時のデフォルト値のまま変更していません。本HDSには`ls.x`が
存在しないため、dired（ファイラ）モードの一覧表示は動作しない可能性がある（**未検証**）。

### MCP アクション（ix68k検証済み）

```
run_command("EM")           # *scratch* バッファで起動
```

| 操作 | MCP アクション | 検証 |
|------|---------------|------|
| 終了（保存せず、確認ダイアログなし） | `key("down","CTRL")` → `key("press","X")` → `key("press","C")` → `key("up","CTRL")` | ✅ |

### 検証メモ

- 検証: 2026-08-27、`tools-host/dev/run_x68000.sh`でGUI起動しix68k確認。
  - `VER`でHuman68k 3.02起動を確認後、`EM`をPATH経由で起動 → スクリーンショットで
    `μEmacs 3.10-j1.43 [1%] () *scratch*` のモードラインとファンクションキーラベル
    （`buf選択`等）が正常表示されることを確認。rcファイルのロードエラー等は発生しなかった
    （`%dir-path`書き換えが正しく機能している証拠）。
  - `^X^C`で確認なく（未編集バッファのため）`C:\>`プロンプトへ復帰することを確認。
  - **導入されているのはSharlEMACS版em.x**であることをファイルサイズ・"HU"マジック・
    ソースコメント内`$Id:`/`$Header:`差異で確認済み（上記「SharlEMACSの『差分』について」参照）。
  - dired・シェル起動（`ESHELL`依存機能）・保存（`^X^S`）は今回未検証。
