---
exe: SXWIN.X
種別: official
作者: Sharp/First Class Technology
版: SX-WINDOW ver3.1（SX SYSTEM version 3.10）
出典: X68000 LIBRARY「SX-WINDOW ver3.1」配布ページ http://retropc.net/x68000/software/sharp/sxwindow/ （無償公開されたシャープのソフトウェア）。同梱の許諾条件に従うこと
判別: -（配布アーカイブ SXWIN311〜316.LZH／SXWN311I〜316I.LZH 収録版のみ確認）。常駐プログラム FSX.X（367,436バイト）とセットで使う
検証状態: ✅ FSX.X常駐→SXWIN.Xでデスクトップ表示・マウス操作をix68kで確認（2026-10-15）。アプリの個別操作は未検証
最終更新: 2026-10-15
---
# SXWIN.X

> SX-WINDOW ver3.1 のウィンドウシステム本体。事前に `FSX.X` を常駐させてから実行すると、GUI デスクトップが立ち上がる。

---

## リファレンス（一次資料要約）

イベントドリブン型でマルチタスク動作する GUI。画面は 1024×1024 ドット・16色のデスクトップを
スクロールして使い、512×512 ドット・65536色のグラフィックとも共存できます。日本語マルチフォント
エディタ「シャーペン」が付属します。

無償公開版では、コンソール実行に必要な `SXCON.X`、ピンボール、サンプル画像・音楽、
インストーラが取り除かれている（配布ページの補足説明による）。そのため、シャーペンのコンソール
機能は使えません。

配布物は5枚のディスク構成。

| ディスク | アーカイブ（ファイル版 / イメージ版） | 内容 |
|---------|------------------------------------|------|
| システム | SXWIN311.LZH / SXWN311I.LZH | `SHELL\`（SXWIN.X ほか本体）、`SYS\FSX.X` ほかドライバ、`ASK\` |
| アプリケーション1 | SXWIN312.LZH / SXWN312I.LZH | 時計・電卓・カレンダー・キャンバス・パターンエディタ等のアプリ（`.X`／`.LB`／`.ENV`） |
| アプリケーション2 | SXWIN313.LZH / SXWN313I.LZH | `FONT\` 下のフォント（`.IFM`、10書体） |
| 辞書 | SXWIN315.LZH / SXWN315I.LZH | `X68K.DIC`、`POST.DIC` |
| 拡張 | SXWIN316.LZH / SXWN316I.LZH | `IFM.X`／`IVM.X`、シャーペン一式、福袋（`SXKEY.X` 等）、外部コマンド開発サンプル |

### 起動の順序

1. `FSX.X` を常駐させる（SX SYSTEM プログラム）。
2. `SXWIN.X` を実行します。ウィンドウシステムが立ち上がり、デスクトップが表示されます。

配布版の `CONFIG.SYS` は、`DEVICE = \SYS\FSX.X` で常駐させたうえで
`SHELL = \SHELL\SXWIN.X -@` を指定し、起動と同時に SX-WINDOW が立ち上がる構成になっている
（`TITLE.X`／`TITLE.SX` でタイトル表示、`USKCG.SYS` も `\SHELL\` から読む）。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/sharp/sxwindow/ （X68000 LIBRARY）。ディスクイメージ版
（`SXWN31xI.LZH`、中身は `SXWIN31x.XDF`）を使うと、`hu68disk` でそのまま読めます。

### インストール（HDSへの導入手順）

ホスト側で完結します。**ix68k を停止してから実行します。**
例では `SYSTEM_HD.HDS`（`\SHELL` が無い状態）を対象にします。

**重複ファイルは入れません。** 次はHuman68k 3.02 導入済みのHDSと内容が完全一致していたため
コピーしない: `HUMAN.SYS` `COMMAND.X` `ASK\ENV1〜5.ASK`、`SYS\` の `ASK68K.SYS` `RSDRV.SYS`
`RAMDISK.SYS` `SRAMDISK.SYS` `FLOAT2.X` `FDDEVICE.X`。
また `CONFIG.SYS` は上書きしない（HDS側の設定を保つ）。`HDﾌｫｰﾏｯﾄ.X`（HD初期化ツール）と
空の `X68\` も入れません。

```sh
HDS=disk/SYSTEM_HD.HDS
H=tools-host/hu68disk/hu68disk
A=arc/retropc.net/x68000/software/sharp/sxwindow     # 無ければ配布元から curl -L --create-dirs -o で取得

# 1. XDFを取り出す（bsdtar が "(null)" と出て終了コードが非0になるが、XDF は展開される）
mkdir -p /tmp/sxwin && cd /tmp/sxwin
for n in 1 2 3 5 6; do bsdtar -xf "$OLDPWD/$A/SXWN31${n}I.LZH" || true; done
cd -

# 2. システムディスク: SHELL\ 一式と SYS\FSX.X
$H mkdir "$HDS" /SHELL
$H copyall /tmp/sxwin/SXWIN311.XDF /SHELL "$HDS" /SHELL
$H list "$HDS" /SYS >/dev/null 2>&1 || $H mkdir "$HDS" /SYS    # Human68k導入済みのHDSには既にある
$H extract /tmp/sxwin/SXWIN311.XDF /SYS/FSX.X /tmp/sxwin/FSX.X
$H copy "$HDS" /tmp/sxwin/FSX.X /SYS/FSX.X

# 3. アプリケーション1: SHELL\ へ
$H copyall /tmp/sxwin/SXWIN312.XDF / "$HDS" /SHELL

# 4. アプリケーション2: SHELL\FONT\ へ
$H mkdir "$HDS" /SHELL/FONT
$H copyall /tmp/sxwin/SXWIN313.XDF /FONT "$HDS" /SHELL/FONT

# 5. 辞書: ルートへ（配布版 CONFIG.SYS が \X68K.DIC を参照する）
$H copyall /tmp/sxwin/SXWIN315.XDF / "$HDS" /

# 6. 拡張: IFM/IVM は SHELL\ へ、シャーペンと福袋は SHELL\ の下へ
$H copyall /tmp/sxwin/SXWIN316.XDF /SHELL "$HDS" /SHELL
$H mkdir "$HDS" /SHELL/ｼｬｰﾍﾟﾝ
$H copyall /tmp/sxwin/SXWIN316.XDF /ｼｬｰﾍﾟﾝ "$HDS" /SHELL/ｼｬｰﾍﾟﾝ
$H mkdir "$HDS" /SHELL/福袋
$H copyall /tmp/sxwin/SXWIN316.XDF /福袋 "$HDS" /SHELL/福袋
```

> 配置先のうち `\SHELL\FONT\`・`\SHELL\ｼｬｰﾍﾟﾝ\`・`\SHELL\福袋\` と辞書のルート配置は、公開ファイル
> 構成からの推定。無償公開版にはインストーラが無いため、元のインストーラが作る配置との一致は
> 未確認（起動確認では SX-WINDOW 本体とデスクトップの表示までを確認している）。
> `ETC\外部コマンド`（開発サンプル）は入れていません。

**AUTOEXEC.BAT**: `PATH` の末尾に `A:\SHELL;` を足すと `SXWIN` と打つだけで起動できる
（`A:` は `A:\BIN\DRIVE A: C:` で `C:` と同じドライブに割り当てられている）。CRLF 改行を保つこと。

```
PATH C:\;C:\SYS;C:\BIN;C:\BASIC2;C:\ETC;C:\EMACS;A:\SHELL;
```

`CONFIG.SYS` への登録は不要。`FSX.X` の常駐と `SXWIN.X` の実行は、下記のとおり手動で行います。

### MCP アクション

**手順は必ず「FSX.X の常駐 → SXWIN.X の実行」の順。** `FSX.X` を先に常駐させていないと
`SXWIN.X` は動作しません。

| 操作 | MCP アクション | 検証 |
|------|---------------|------|
| FSX.X を常駐させる | `paste_text("\\SYS\\FSX.X\n")` | ✅ |
| ウィンドウシステムを起動する | `paste_text("\\SHELL\\SXWIN.X\n")` | ✅ |

- `FSX.X` の常駐に成功すると「SX SYSTEM プログラムが常駐しました」と表示されます。
- `SXWIN.X` を実行すると画面がデスクトップに切り替わる。左端に SX ロゴ・ドライブ・ごみ箱のアイコンが並びます。
- 画面切り替えのため、2回目の `paste_text` は「入力確認ができませんでした」のタイムアウトエラーを返すが、
  起動自体は成功しています。`screenshot` で表示を確認します。
- `FSX.X` は一度常駐すれば再実行しない（リセット後は再度必要）。

### 検証メモ

- 検証: 2026-10-15、`SYSTEM_HD.HDS` へ導入し、ix68kで `\SYS\FSX.X` → `\SHELL\SXWIN.X` の順に実行。
  デスクトップとドライブトレイのウィンドウが表示され、ix68kのマウスで操作できた。
- マウス移動量が半分になる問題は ix68k 側の不具合で修正済み。それ以前のビルドでは
  SX-WINDOW 上のカーソルが画面下端まで届かありませんでした。
- 未検証: 個別アプリ（シャーペン・キャンバス等）の操作、SX-WINDOW の終了手順、辞書（日本語入力）、
  配布版 `CONFIG.SYS` による自動起動。
- 同梱の許諾条件（`許諾条件.txt`）は、配布条件を確認するため目を通すこと。HDS へは入れていません。
