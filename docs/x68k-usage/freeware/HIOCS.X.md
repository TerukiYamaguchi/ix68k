---
exe: HIOCS.X
種別: freeware
作者: TcbnErik（原作: Y.Nakamura「HIOCS.X」）
版: HIOCS PLUS（HIOCS.X version 1.10+16.22）
出典: 添付HIOCS.DOC/HIOCSVEC.DOC（原作マニュアル）。配布元
   https://github.com/kg68k/hiocs-plus（改造部分の追加スイッチ`-e -f -j -l -m -u -r`等は
   同リポジトリのhiocs_plus.txtによる。ChangeLog.mdに1.10+16系列の修正履歴あり）
判別: -
検証状態: ✅ CONFIG.SYS常駐（IOCS.Xから置き換え）・起動確認済み。フォントファイル切替もix68k確認済み。スイッチ個別検証は未着手
最終更新: 2026-09-03
---
# HIOCS.X (HIOCS PLUS)

> IOCS.Xを元にコンソール・グラフィックIOCS/DOSコール処理を高速化し、機能拡張したデバイスドライバ。HIOCS.DOC「IOCS.Xを元にコンソール関係を中心とした高速化・機能拡張を行なったドライバ」より。IOCS.Xの代替として組み込みます。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式（CONFIG.SYS常駐・コマンドライン常駐共通）:

```
HIOCS [スイッチ...]
```

CONFIG.SYSでは`DEVICE = HIOCS.X <スイッチ>`、コマンドラインでは`HIOCS <スイッチ>`と
直接入力することで組み込めます。スイッチキャラクタは`-`のみ（HIOCS PLUSでは`/`不可、
原作HIOCS.DOCの`/`表記は旧仕様）。`-x0`/`+x`のように指定すると機能を解除できます。

| スイッチ | 説明 | 検証 |
|-----------|------|----|
| `-c[文字]` | 二バイト文字未定義コードのフォント指定。省略時は`※`、`-c[漢字]`でその文字、`-c`のみでメッシュパターン、16進32バイト指定で任意パターンに変更 | 📖 |
| `-d` | DOSコンソール出力を高速化しない | 📖 |
| `-e` | condrv用ESCシーケンス（ESC 0〜3）を無効にする | 📖 |
| `-f` | フォントパターンの初期化。拡張フォントバッファを解放しROMパターンに戻す（原作版とは意味が異なる） | 📖 |
| `-g` | IOCS/DOSコンソール及びマウスIOCSを高速化しない（グラフィックIOCSのみ高速化） | 📖 |
| `-j` | 二バイト文字未定義コードのサポートを行わない（常駐時のみ有効） | 📖 |
| `-l[n]` | JISコード`$7621`〜`$7e7e`のフォントモード指定。`-l0`＝従来通り外字/未定義、`-l1`＝ユーザーフォントを全角表示 | 📖 |
| `-m` | ROM IOCSへのパッチ当てのみ行う（常駐時のみ有効） | 📖 |
| `-ms[n]` | マウスカーソル移動速度指定（1〜4、大きいほど高速）。省略時/`-ms2`で標準速度、`-ms0`でマウスIOCSを標準に戻す | 📖 |
| `-mh` | SCCクロックを7.5MHz系とみなしてマウスポート周波数を設定（意味を理解できない場合は使用禁止と明記） | 📖 |
| `-s` | 半角文字(`\`,`~`,`\|`)のスイッチ指定を無視する | 📖 |
| `-u[n]` | JISコード`$2921`〜`$2f7e`のフォントモード指定。`-u0`＝ROM IOCS準拠、`-u1`＝TC.X/HST.X準拠、`-u2`＝全域を全角表示 | 📖 |
| `-v` | ビープ音をビジュアルベルにする | 📖 |
| `-r` | フックしたベクタを解除しデバイスを切り離す（コマンドライン組み込み時は常駐部を解放） | 📖 |

コマンドラインの末尾にオプションでないトークン（`-`/`+`で始まらない引数）を渡すと、
それをフォントファイル名として読み込み、ユーザーフォントデータに切り替える
（`-f`はこれと組み合わせ可能——`-f`で一旦ROMパターンに戻してから新しいフォント
ファイルを読み込む、という順序で処理される）。

### 拡張機能（condrv.sys併用時）

condrv.sysが組み込まれている場合、以下のESCシーケンスが利用可能（`-e`で無効化）。

| シーケンス | 効果 | 検証 |
|-----------|------|----|
| `ESC 0` | バックスクロールバッファへの保存を中断 | 📖 |
| `ESC 1` | バックスクロールバッファへの保存を再開 | 📖 |
| `ESC 2` | システムステータス表示を停止 | 📖 |
| `ESC 3` | システムステータス表示を再開 | 📖 |

condrv.sysとの併用はv1.09c+08a以降を先に組み込み、その直後にHIOCS.Xを組み込む
必要がある（それ以外の組み合わせ・順序は非保証）。

### 対象IOCS/DOSコールベクタ

差し替え対象は原作HIOCS.DOC/hiocs_plus.txt「ベクタ一覧」参照。グラフィックIOCS
（`_DRAWMODE`〜`_TXREV`）、コンソールIOCS（`_B_PUTC`〜`_B_PUTMES`等、`-g`指定時は
変更しない）、DOSコール（`_PUTCHAR`/`_PRINT`/`_WRITE`等、`-d`/`-g`指定時は変更しない）、
マウスIOCS（`_MS_INIT`等、`-g`/`-ms0`指定時は変更しない）、ROM IOCSのバグ修正
（`_GPALET`全バージョン、`_DMAMOVE`/`_MS_PATST`は該当ROM版のみ）を含みます。

---

## ix68kでの利用

### 入手先

https://github.com/kg68k/hiocs-plus の`archives/HIOCS16.LZH`（プリビルド版バイナリ
セット、v1.10+16.22相当）を使用。ソース版`archives/HIOCSSRC.LZH`はUTF-8ソースで
u8tosj経由のビルドが必要なため未取得（バイナリ配布で足りるため）。

**`arc/github.com/kg68k/hiocs-plus/archives/HIOCS16.LZH`にローカル保存済み**
（2026-09-03取得）。再検証・再インストール時はGitHubへ再アクセスせず、まずこちらを使うこと。

### インストール（SYSTEM_HD.HDSへの導入手順）

**注意: 本HDSには元々`DEVICE = \SYS\IOCS.X`（標準IOCSドライバ）が登録済みでした。
HIOCS.XはIOCS.Xを土台にした上位互換ドライバのため、共存させず`IOCS.X`→`HIOCS.X`に
置き換える形で導入した**（両方ロードすると同一ベクタへの二重パッチで不整合を起こす
おそれがあるため。ユーザー指示で置き換え方式を採用）。

```sh
# 1. ダウンロード（arc/にローカル保存済みならそれを使う）
cp arc/github.com/kg68k/hiocs-plus/archives/HIOCS16.LZH ./HIOCS16.LZH
# 無ければ: curl -LO https://raw.githubusercontent.com/kg68k/hiocs-plus/main/archives/HIOCS16.LZH

# 2. 展開（unar、要 brew install unar。LZH形式）
unar -o ./hiocs_extracted HIOCS16.LZH
# → ./hiocs_extracted/HIOCS16/{HIOCS.X, HIOCS.DOC, HIOCSVEC.DOC}

# 3. HIOCS.XをHDSの\SYSへコピー（既存IOCS.Xと同じ場所、hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./hiocs_extracted/HIOCS16/HIOCS.X /SYS

# 4. ドキュメントはHDSの\DOC\HIOCSへコピー
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/HIOCS
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./hiocs_extracted/HIOCS16/HIOCS.DOC /DOC/HIOCS
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./hiocs_extracted/HIOCS16/HIOCSVEC.DOC /DOC/HIOCS

# 5. CONFIG.SYSの `DEVICE = \SYS\IOCS.X` を `DEVICE = \SYS\HIOCS.X` に置き換える
#    （CRLF改行・末尾EOFマーカー($1A)を保持したまま書き換えること。詳細はperl -pe等で
#    バイナリ的に置換し、hu68disk extract→編集→hu68disk copyで書き戻す）
```

スイッチは指定なし（デフォルト動作）で導入。既存のIOCS.X行と同じ位置・同じ形式
（引数なし）を踏襲しました。

### MCP アクション

起動例（コマンドラインからの一時的な組み込み・確認用。CONFIG.SYS常駐が既定）:

```
run_command("HIOCS")     # スイッチなしで再組み込み（既に常駐中なら状態表示のみ）
run_command("VER")       # Human68kバージョン確認（HIOCS組み込み後も通常応答することの確認）
```

起動時のバナー（`screenshot`で確認可能）:

```
HIOCS version 1.10+16  Copyright 1990-93 SHARP / Y.Nakamura
```

### 検証メモ

- 2026-09-03: `archives/HIOCS16.LZH`から`HIOCS.X`/`HIOCS.DOC`/`HIOCSVEC.DOC`を展開し、
  `\SYS\HIOCS.X`・`\DOC\HIOCS\`へ導入。CONFIG.SYSの`DEVICE = \SYS\IOCS.X`を
  `DEVICE = \SYS\HIOCS.X`に置換（CRLF/EOFマーカー保持を`xxd`差分で確認済み）。
- 2026-09-03: エミュレータをリセットして起動シーケンスを確認。「HIOCS version 1.10+16
  Copyright 1990-93 SHARP / Y.Nakamura」のバナーが表示され、`C:\>`プロンプト到達後
  `VER`コマンドが正常応答（`Human68k version 3.02`）することを確認。
- 2026-09-03: フォントファイル切り替え機能をix68k検証。`C:\SYS\VAX.FON`（4096バイト＝
  `FONTSIZE0`＝半角文字専用サイズ）を`run_command("HIOCS C:\SYS\VAX.FON")`で読み込み、
  「ユーザーフォントデータに切り替えます」表示後、半角文字`\`(0x5C)のグリフが
  ROMフォントの円記号「¥」から実際のバックスラッシュ「\」に変わることを確認
  （`TYPE C:\STARTUP.ENV`の`path=\;\SYS;...`行で比較）。ファイルサイズが
  `FONTSIZE0`丁度のため漢字部分はROMのままで変化なし（日本語文書`HIOCS.DOC`表示では
  差が出ないことも確認）。`HIOCS -f`でROMパターンへ復帰することも確認。
- 未検証: 個別スイッチ（`-c`/`-d`/`-e`/`-g`/`-j`/`-l`/`-m`/`-ms`/`-mh`/`-s`/`-u`/`-v`/
  `-r`）、condrv.sys併用時のESCシーケンス拡張、漢字部分を含むフォントファイル
  （`FONTSIZE01`/`FONTSIZE012`/`FONTSIZE0123`サイズ）の切り替え。
- 出典アーカイブ: `HIOCS16.LZH`（22.8KB、`HIOCS.X`/`HIOCS.DOC`/`HIOCSVEC.DOC`を同梱）。
