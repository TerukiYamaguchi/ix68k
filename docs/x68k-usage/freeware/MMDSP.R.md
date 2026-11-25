---
exe: MMDSP.R
種別: freeware
作者: Miahmie / Gao / Fu-. / BEL.（オリジナル）、立花えりりん（立花えり子）（Rel.31 patchlevel 改造）
版: v0.29 Rel.31 patchlevel 2（`mmr31p1.zip`の`mmdsp.r`に`mmr31p2.zip`の`mmdsp.bfd`を適用、74826 bytes）
出典: `arc/x68tools/SOUND/PLAYER/mmr31p1.zip`（本体・ソース・README.DOC）/
  `arc/x68tools/SOUND/PLAYER/mmr31p2.zip`（p1→p2差分）/
  操作解説は`arc/x68tools/SOUND/PLAYER/MMDSP029.LZH`収録の`MMDSP029.doc`（v0.29のマニュアル）/
  Rel.31のオプション一覧は`mmr31p1.zip`同梱ソース`src/mmd_init.s`のUSAGE_MES
判別: `arc/x68tools/SOUND/PLAYER/`に複数版あり。`MMDSP030.LZH`＝0.30β（MMDSP.r 52330 bytes,
  1994-07-27）、`mmdspz35.lzh`＝Z-MUSIC ver3対応版（53734 bytes, 1997-04-17）、
  `MMDSP029.LZH`＝0.29（50746 bytes, 1994-04-30）。**日付上の最新は本ファイルが扱う
  Rel.31 patchlevel 2（1999-08-07）**で、0.30βは1994年で更新が止まっている系統
検証状態: ✅ インストール・patchlevel 2パッチ適用・起動/終了をix68k確認済み
  （起動バナー "MMDSP Ver. 0.29 Rel.31"、DRIVER欄にZMUSIC・ADPCM欄にPCM8Aを自動認識）。
  ZDFファイルの演奏動作も確認済み（[LZZ.R](LZZ.R.md)によるZDF展開を経由）。
  各コマンドラインオプションは未検証
最終更新: 2026-09-03
---
# MMDSP.R

> 常駐した音楽ドライバの演奏状態をグラフィカルに表示するマルチミュージックステータス
> ディスプレイ兼セレクタ。鍵盤表示・スペクトラムアナライザ・ベロシティ表示・ファイル
> セレクタを1画面に統合し、MXDRV / MADRV / MLD / RCD / ZMUSIC / MCDRV の各ドライバに対応します。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式（出典: `src/mmd_init.s` USAGE_MES。`MMDSP029.doc`「使用法」も併用）:

```
MMDSP [オプション] [ディレクトリ名／演奏ファイル名]
```

ディレクトリ名を渡すとそこへカレントを移して起動、ファイル名を渡すとそのまま演奏を
開始します。環境変数`mmdspcmd`の中身がコマンドラインの末尾に付け足されます。

| オプション | 意味 |
|-----------|------|
| `-<name>` | 起動時に使うドライバを固定します。`-mxdrv` `-madrv` `-mld` `-rcd` `-zmusic` `-mcdrv`のいずれか。省略時はMXDRV→MADRV→MLD→RCD→ZMUSICの順に探し、最初に見つかったものを使う |
| `-g[n]` | 画面表示モード。`0`=表示なし、`1`=テキスト優先、`2`=半透明、`3`=グラフィック優先 |
| `-a[bbbb]` | AUTO演奏モードで起動します。`bbbb`はREP./INTRO/ALLDIR/PROG.のon/offを並べたビット指定（例: `-a1010`） |
| `-s[bbbb]` | SHUFFLE演奏モードで起動します。ビット指定の形式は`-a`と同じ |
| `-n` | セレクタ画面を使わない（ファイル検索なし。AUTO/SHUFFLE演奏も不可になる） |
| `-f` | テキストVRAMが使用中でも構わず使う |
| `-k[key1,key2]` | 常駐モードにし、key1+key2の同時押しで画面呼び出し／待機を切り替えます。キー指定はスキャンコードの16進（既定はXF4+XF5、例: `-k70,55`＝SHIFT+XF1）。演奏ドライバが未常駐でも常駐だけはできる |
| `-r` | 常駐を解除する |
| `-t[sec]` | 無操作で画面がトーンダウンするまでの秒数（Rel.31で追加） |
| `-c[n]` | トーンダウン時の明るさ（0〜15、Rel.31で追加） |
| `-i[n]` | 使う割り込みを指定する（1〜4、Rel.31で追加）。既定はTIMER-A→RASTER→VDISP→TIMER-Dの順で空きを自動選択。`-i0`で自動選択に戻せる（patchlevel 1にあったアドレスエラーの不具合はここで修正済み） |
| `-p` | パレットアニメーションを使わない（Rel.31で追加） |
| `-nodef` | 定義ファイルを読み込まずに起動する（Rel.31で追加） |

### 起動後の操作

出典: `MMDSP029.doc`「起動後」の操作一覧のうち主要なものを抜粋（マウス操作は原資料参照）。

| 操作 | キー手順 |
|------|---------|
| カーソル移動 | `[↑]` `[↓]`（テンキー`8` `2`） |
| カーソル高速移動 | `[ROLLUP]` `[ROLLDOWN]` |
| 演奏／選択 | `[RETURN]`（`[ENTER]`） |
| 親ディレクトリへ | `[UNDO]`（`[.]` `[0]`） |
| ルートへ | `[¥]` |
| ドライブ移動 | `[←]` `[→]`（テンキー`4` `6`） |
| DOCビューワ起動 | `[TAB]` |
| 曲の始めから再演奏 | `[F6]` |
| 演奏の中断／再開 | `[F7]` |
| フェードアウト | `[F8]` |
| スロー | `[F9]` |
| AUTO／SHUFFLE／REPEAT／INTRO／ALLDIR／PROG.モード切替 | `[A]` `[S]` `[Z]` `[X]` `[C]` `[V]` |
| ディスクイジェクト | `[BREAK]` |

---

## ix68kでの利用

### 入手先

**配布元URL不明**（リポジトリ内アーカイブコレクション由来。掲載元は「うちのはーどぢすく」、
掲載者は立花えりりん）。以下2ファイルを`arc/`に保存済み。

- `arc/x68tools/SOUND/PLAYER/mmr31p1.zip` … v0.29 Rel.31 patchlevel 1（1999-01-11）。
  実行ファイル`mmdsp.r`（74638 bytes）・全ソース・`README.DOC`を同梱。
- `arc/x68tools/SOUND/PLAYER/mmr31p2.zip` … patchlevel 1 → 2 の差分（1999-08-08）。
  `mmdsp.bfd`（[BUP.X](BUP.X.md)用バイナリ差分）と`mmdsp.dif`（ソース差分）。
  **単体では使えず`mmr31p1.zip`が必要**。

操作マニュアルは同梱されていないため、`MMDSP029.LZH`収録の`MMDSP029.doc`（v0.29のマニュアル）
を参照します。Rel.31で追加されたオプション（`-t` `-c` `-i` `-p` `-nodef` `-mcdrv`）は
同doc未収録のため、ソース`src/mmd_init.s`のUSAGE_MESを一次資料としました。

### インストール（SYSTEM_HD.HDSへの導入手順）

```sh
# 1. 展開（unar、要 brew install unar）
unar -e SHIFT-JIS -o ./mmdsp_extracted arc/x68tools/SOUND/PLAYER/mmr31p1.zip
unar -e SHIFT-JIS -o ./mmdsp_extracted arc/x68tools/SOUND/PLAYER/mmr31p2.zip
# zip内のディレクトリパーミッションが読み取り専用で展開されることがあるため必要なら
chmod -R u+rwX ./mmdsp_extracted

# 2. 本体を \BIN へ、ドキュメントを \DOC\MMDSP\ へ（ソースは転送しない）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./mmdsp_extracted/mmdsp-0.29-31-1/mmdsp.r /BIN
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/MMDSP
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./mmdsp_extracted/mmdsp-0.29-31-1/README.DOC /DOC/MMDSP
# v0.29のマニュアル・更新履歴（別アーカイブ）
unar -e SHIFT-JIS -o ./mmdsp029 arc/x68tools/SOUND/PLAYER/MMDSP029.LZH
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./mmdsp029/MMDSP029/MMDSP029.doc /DOC/MMDSP
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./mmdsp029/MMDSP029/verup.doc /DOC/MMDSP

# 3. patchlevel 2 適用の材料を作業用 \TMP へ（エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./mmdsp_extracted/mmdsp-0.29-31-1/mmdsp.r /TMP
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./mmdsp_extracted/mmdsp-0.29-31-2/mmdsp.bfd /TMP
```

```
# 4. エミュレータ起動後、ix68k内で（MCP run_command）
CD \TMP
BUP MMDSP.BFD
# → `mmdsp.r' ... completed.（元ファイルは自動で \TMP\orig へバックアップされる）
#    mmdsp.r が 74638 → 74826 bytes・日付 99-08-07 になれば成功
COPY MMDSP.R C:\BIN\MMDSP.R
```

```sh
# 5. エミュレータ停止後、作業ファイルを片付ける
tools-host/hu68disk/hu68disk del   disk/SYSTEM_HD.HDS /TMP/orig/mmdsp.r
tools-host/hu68disk/hu68disk rmdir disk/SYSTEM_HD.HDS /TMP/orig
tools-host/hu68disk/hu68disk del   disk/SYSTEM_HD.HDS /TMP/mmdsp.r
tools-host/hu68disk/hu68disk del   disk/SYSTEM_HD.HDS /TMP/mmdsp.bfd
```

`bup.x`はコピー元とタイムスタンプが一致しないと拒否するが、アーカイブから展開した
ファイルを`hu68disk copy`で転送する分にはオリジナルのタイムスタンプが保たれるため
`-NT`は不要（[ZMUSIC.X.md](ZMUSIC.X.md)「2.08e パッチ適用手順」と同じ条件）。

### MCP アクション

| 操作 | MCP アクション | 検証 |
|------|---------------|------|
| 起動 | `paste_text("MMDSP\n")` — MMDSPが画面を奪うためツールは**タイムアウトエラーを返すが起動自体は成功する**。`run_command`はプロンプトが戻らないため使わない | ✅ |
| 終了 | `key(action="press", key="ESCAPE", hold_ms=80)` → `key_wait(action="check")`が`WAITING=1`を返せば復帰（またはCONTROL PANELの`MMDSP QUIT`をクリック） | ✅ |

### 検証メモ

- 2026-08-18: `disk/SYSTEM_HD.HDS`へ導入。`mmr31p1.zip`の`mmdsp.r`を`\BIN`へ配置後、
  ix68k上で`BUP MMDSP.BFD`によりpatchlevel 2を適用（74638→74826 bytes、日付99-08-07）し、
  `\BIN\MMDSP.R`へコピー。ドキュメント3点を`\DOC\MMDSP\`へ配置。作業用`\TMP`は片付け済み。
- 起動確認: `C:¥>`で`MMDSP`を実行し、コントロールパネル画面
  （バナー "MMDSP Ver. 0.29 Rel.31 COPYRIGHT 1991-97 Miahmie, Gao / Fu-., BEL."）が表示。
  DRIVER欄が`ZMUSIC`、ADPCM欄が`PCM8A`となり、常駐中の[ZMUSIC.X](ZMUSIC.X.md)・
  [PCM8A.X](PCM8A.X.md)を自動認識することを確認。セレクタにC:¥のディレクトリ一覧も表示されました。
- 終了確認: `[ESC]`でCOMMAND.Xのプロンプトへ復帰。当初「終了後に画面へゴミが残る」現象が
  あったが、2026-08-19に修正済み（`docs/ix68k-internals/support/sprite_support.md`参照）。MMDSPは
  通常起動パス（`INIT`）ではビデオコントローラR2・`$EB0808`を復元せず終了する（復元するのは
  `-k`常駐モードの`KEEPJOB_CORE`のみ）が、実機では768×512に戻る際にIPLROMが
  `$EB0810`へ非対応モード値（`$FF`等）を書くためSP/BG面が表示されません。2026-08-19時点では
  この機序をCRTC R20の解像度ビットで代用判定していたが、CRTC直叩きの正方形ドットモード
  （otoko.c等）ではR20とスプライトコントローラ自身の設定が食い違うため誤判定を招き、
  2026-09-10に`$EB0810`自身の値のみで判定する方式へ訂正した（`docs/ix68k-internals/design/sprite_design.md`）。
- 演奏確認（2026-09-03、ユーザー実施）: 手元のZDFファイルをセレクタから選択して演奏し、
  動作することを確認（[LZZ.R](LZZ.R.md) v0.57a導入後）。
- **未検証**: ZDF以外の形式（MDF/ZMD等）での演奏、各コマンドラインオプション、
  常駐モード（`-k`／`-r`）、DOCビューワ。
