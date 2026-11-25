# X68000 利用ガイド・ソフトウェア操作リファレンス

X68000 を使い始めるための利用ガイド、アプリ開発資料、MCP対応のAIエージェント向け資料、および
X68000 上で動くソフトウェアの**操作リファレンス**をまとめます。
エミュレータ本体の開発資料は `docs/ix68k-internals/` で管理します。

ビルド・起動・MCP操作・資料の使い方の入口は [ix68k マニュアル](manual.md) にあります。
X68000を初めて使う場合は、まず [guide/](guide/README.md) を読みます。アプリ開発は
[development/](development/README.md)、MCP対応のAIエージェントによる操作は
[mcp/](mcp/README.md) を参照します。

## エージェント向け参照ルール

ユーザーが「<コマンド>を起動して／終了して／保存して …」と指示したら:

1. **逆引きインデックス**（下の表）で実行ファイル名 → ファイルパスを特定
2. 該当ファイルの「MCP アクション」列を読んで MCP ツールを実行

例: 「ED.X を終了して」→ `official/ED.X.md` の「保存せず終了」行の MCP アクションを実行。

複数ファイルがヒットした場合は `判別:` frontmatter で使用版を確認してから実行すること。

## ソフトウェアバンドルインストール

ユーザーが「開発環境一式をインストールして」等、複数ソフトをまとめて指す指示をしたら、
該当バンドルに含まれる全ソフトを、各マニュアルの「## インストール」セクション通り
**下記の順序で**インストールします。

### 音源ドライバ一式（ADPCM／Z-MUSIC 演奏環境）

ユーザーが「**音源ドライバ一式をインストールして**」と言ったら、以下を**この順序で**導入します。

| 順序 | ソフト | マニュアル | 役割 |
|------|--------|-----------|------|
| 1 | PCM8A.X | [freeware/PCM8A.X.md](freeware/PCM8A.X.md) | リアルタイムADPCM多重再生ドライバ（PCM8+MPCM相当） |
| 2 | ZMUSIC.X | [freeware/ZMUSIC.X.md](freeware/ZMUSIC.X.md) | 音源ドライバ Z-MUSIC ver2（2.08eパッチ適用まで実施） |
| 3 | MZP.X 一式 | [freeware/MZP.X.md](freeware/MZP.X.md) | マルチミュージックプレーヤー本体＋変換ツール5本（`MXtoZ.x` `NDtoZ.x` `RCtoZ.x` `MLtoZ.x` `SDtoZ.x`）を`\BIN`へ |
| 4 | LZZ.R | [freeware/LZZ.R.md](freeware/LZZ.R.md) | MDF/ZDF形式の展開・圧縮ツール。MZP.XがMDF/ZDF演奏データを扱う際に内部から利用 |

**PCM8AはZMUSICより先に常駐させること**（Z-MUSICがADPCMを多重発声するにはPCM8系ドライバが
先に常駐している必要があるため）。

全て導入後、**AUTOEXEC.BATの末尾（EOFマーカー`$1A`の直前）へ以下の2行をオプションなしで
追記する**（**CRLF改行で保存すること**——開発環境一式と同じ注意が適用される）:

```
PCM8A
ZMUSIC
```

MZPは常駐型ではないため、AUTOEXEC.BATには登録しない（演奏時にコマンドラインから起動する）。

また、**CONFIG.SYSに`OPMDRV3.X`の`DEVICE=`行が残っていたら削除する**（FM音源(OPM)を
占有するドライバで、OPMを制御するZMUSICと併用しないため）。手順は
[mcp/provisioning/sound_driver_install.md](mcp/provisioning/sound_driver_install.md)
「Step 5-2」を参照。

### 開発環境一式（C言語クロス開発環境）

| 順序 | ソフト | マニュアル | 役割 |
|------|--------|-----------|------|
| 1 | HAS.X | [freeware/HAS.X.md](freeware/HAS.X.md) | アセンブラ |
| 2 | HLK.R | [freeware/HLK.R.md](freeware/HLK.R.md) | リンカ |
| 3 | LIBC | [freeware/LIBC.md](freeware/LIBC.md) | Cライブラリ・ヘッダ |
| 4 | GCC.X | [freeware/GCC.X.md](freeware/GCC.X.md) | Cコンパイラ |
| 5 | MAKE.X | [freeware/MAKE.X.tachibana-v3.79.md](freeware/MAKE.X.tachibana-v3.79.md) | ビルド自動化（GNU Make 3.79移植） |

全て導入後、**AUTOEXEC.BATに以下の環境変数を追記する**（`GCC.X.md`ix68k検証済み。
**追記はCRLF改行で保存すること**——LFのまま書き戻すとHuman68k側で表示が崩れる/一部
ツールが正しく解釈しない可能性があります。`docs/x68k-usage/mcp/provisioning/hdd_install.md`のCRLF注意も参照）:

```
SET include=C:\INCLUDE
SET lib=C:\LIB
SET GCC_AS=HAS
SET GCC_LINK=HLK
SET GCC_LIB=.a
SET DOSEQU=C:\INCLUDE\doscall.equ
SET GCC_NO_XCLIB=1
```

（対象HDSのドライブレターが`C:`でない場合はパスを読み替えます。`GCC_NO_XCLIB`の意味は
`GCC.X.md`「⚠️ GCC_NO_XCLIB」参照——LIBCのUnix風命名規則`libc.a`等を使うために必須。）

導入後の動作確認は`GCC.X.md`「動作確認の再現手順」（`puts`版・`printf`版Hello World
のコンパイル→実行）を参照。

## ディレクトリ構成

```
docs/x68k-usage/
├── README.md            ← このファイル（インデックス）
├── manual.md            ← ix68k マニュアル（導入・起動・MCP・資料の使い方の入口）
├── _TEMPLATE.md         ← 新規作成時のひな型
├── guide/                ← 初心者向けの通常利用ガイド
├── development/          ← X68000アプリ開発資料
├── mcp/                  ← MCP対応のAIエージェントによる操作資料
├── official/             ← Human68k 付属コマンド（Sharp/Hudson 純正）。ED.X.md など
└── freeware/             ← フリーウェア
```

**ファイル命名規則:**
- 基本: `<exeそのまま(大文字)>.md`（例: `ED.X` → `ED.X.md`、`INSTALL.BAT` → `INSTALL.BAT.md`）
- 同名異版: `<exeそのまま(大文字)>.<作者タグ>-v<ver>.md`（例: `MENU.X.tanaka-v3.md`）
  - 真の識別は frontmatter の `exe` / `作者` / `版` / `判別` が正

**拡張子対応:** `.X`（外部コマンド）、`.R`（リロケータブル）、`.SYS`（ドライバ）、`.BAT`（バッチ）すべて同規則。

## 管理規約

1. **1 実行ファイル（版）= 1 ファイル**。作者・版が違えば別ファイル。
2. **COMMAND.X 内部コマンド**（DIR / TYPE / COPY 等）は `official/COMMAND.X_builtin.md` 1 枚に集約。
3. **記憶で埋めない**。必ず一次資料（マニュアル / README.TXT 等）かix68k検証に基づきます。
4. **OCR 誤読に注意**: `$`→`S`（`ED.$$$`→`ED.SSS`）、`*`/全角半角など。X68000 は `$FF` や `$$$` が頻出するのでix68k表示と突き合わせること。
5. 新規追加時は逆引きインデックスも更新します。
6. **`📖`（一次資料参照）の記述は、原文の言い回しをそのまま転記せず、自分の言葉で要約・
   再構成する**。数値・スイッチ名・デフォルト値・ビット割り当てなどの事実部分はそのまま
   維持してよい（事実自体には著作権が及ばない）が、説明文は独自の構成・言い回しで書きます。
   ユーザーへの配布を前提とするため（著作権法上の引用の要件——明瞭区別性・主従関係——を
   単純な転記では満たしにくい）。個々のファイルにこの注記を書く必要はありません。
7. **ファイル内は「リファレンス（一次資料要約）」と「ix68kでの利用」の2節に
   分ける**（`_TEMPLATE.md`参照）。起動・コマンドラインオプション等の一次資料由来の内容は
   前者に、入手先・インストール手順・MCPアクション・ix68k検証結果など本プロジェクト固有の
   内容は後者にまとめます。

---

## 逆引きインデックス（実行ファイル名 → マニュアルファイル）

| 実行ファイル | 種別 | ファイル | 検証状態 |
|-------------|------|---------|---------|
| COMMAND.X | official | [official/COMMAND.X.md](official/COMMAND.X.md) | 📖 マニュアル参照のみ |
| ED.X | official | [official/ED.X.md](official/ED.X.md) | ✅ 起動/終了（スイッチは OCR 待ち） |
| SWITCH.X | official | [official/SWITCH.X.md](official/SWITCH.X.md) | ✅ 起動/ESC終了/P8値確認（パラメータ操作は📖） |
| CASE.X | official | [official/CASE.X.md](official/CASE.X.md) | 📖 マニュアル参照のみ |
| COPYALL.X | official | [official/COPYALL.X.md](official/COPYALL.X.md) | 📖 マニュアル参照のみ |
| DRIVE.X | official | [official/DRIVE.X.md](official/DRIVE.X.md) | 📖 マニュアル参照のみ |
| MOVE.X | official | [official/MOVE.X.md](official/MOVE.X.md) | 📖 マニュアル参照のみ |
| PRINT.X | official | [official/PRINT.X.md](official/PRINT.X.md) | 📖 マニュアル参照のみ |
| PROCESS.X | official | [official/PROCESS.X.md](official/PROCESS.X.md) | 📖 マニュアル参照のみ |
| TERM.X | official | [official/TERM.X.md](official/TERM.X.md) | 📖 マニュアル参照のみ |
| TOUCH.X | official | [official/TOUCH.X.md](official/TOUCH.X.md) | 📖 マニュアル参照のみ |
| TREE.X | official | [official/TREE.X.md](official/TREE.X.md) | 📖 マニュアル参照のみ |
| WHERE.X | official | [official/WHERE.X.md](official/WHERE.X.md) | 📖 マニュアル参照のみ |
| MAKE.X | official | [official/MAKE.X.md](official/MAKE.X.md) | 📖 マニュアル参照のみ |
| MAKE.X (GNU Make 3.79 human68k-1.1) | freeware | [freeware/MAKE.X.tachibana-v3.79.md](freeware/MAKE.X.tachibana-v3.79.md) | ✅ SYSTEM_HD.HDSの`\BIN`へ導入完了。起動確認は未実施 |
| ATTRIB.X | official | [official/ATTRIB.X.md](official/ATTRIB.X.md) | 📖 マニュアル参照のみ |
| BACKUP.X | official | [official/BACKUP.X.md](official/BACKUP.X.md) | 📖 マニュアル参照のみ |
| BIND.X | official | [official/BIND.X.md](official/BIND.X.md) | 📖 マニュアル参照のみ |
| CACHE.X | official | [official/CACHE.X.md](official/CACHE.X.md) | 📖 マニュアル参照のみ |
| CHKDSK.X | official | [official/CHKDSK.X.md](official/CHKDSK.X.md) | 📖 マニュアル参照のみ |
| COPY2.X | official | [official/COPY2.X.md](official/COPY2.X.md) | 📖 マニュアル参照のみ |
| DISKCOPY.X | official | [official/DISKCOPY.X.md](official/DISKCOPY.X.md) | 📖 マニュアル参照のみ |
| DUMP.X | official | [official/DUMP.X.md](official/DUMP.X.md) | 📖 マニュアル参照のみ |
| FASTIO.X | official | [official/FASTIO.X.md](official/FASTIO.X.md) | 📖 マニュアル参照のみ |
| FASTOPEN.X | official | [official/FASTOPEN.X.md](official/FASTOPEN.X.md) | 📖 マニュアル参照のみ |
| FASTSEEK.X | official | [official/FASTSEEK.X.md](official/FASTSEEK.X.md) | 📖 マニュアル参照のみ |
| FC.X | official | [official/FC.X.md](official/FC.X.md) | 📖 マニュアル参照のみ |
| FIND.X | official | [official/FIND.X.md](official/FIND.X.md) | 📖 マニュアル参照のみ |
| FORMAT.X | official | [official/FORMAT.X.md](official/FORMAT.X.md) | 📖 マニュアル参照のみ |
| KEY.X | official | [official/KEY.X.md](official/KEY.X.md) | 📖 マニュアル参照のみ |
| MORE.X | official | [official/MORE.X.md](official/MORE.X.md) | 📖 マニュアル参照のみ |
| PR.X | official | [official/PR.X.md](official/PR.X.md) | 📖 マニュアル参照のみ |
| RECOVER.X | official | [official/RECOVER.X.md](official/RECOVER.X.md) | 📖 マニュアル参照のみ |
| RESTORE.X | official | [official/RESTORE.X.md](official/RESTORE.X.md) | 📖 マニュアル参照のみ |
| SCREEN | official | [official/SCREEN.md](official/SCREEN.md) | 📖 マニュアル参照のみ |
| SORT.X | official | [official/SORT.X.md](official/SORT.X.md) | 📖 マニュアル参照のみ |
| SPEED.X | official | [official/SPEED.X.md](official/SPEED.X.md) | 📖 マニュアル参照のみ |
| SUBST.X | official | [official/SUBST.X.md](official/SUBST.X.md) | 📖 マニュアル参照のみ |
| SYS.X | official | [official/SYS.X.md](official/SYS.X.md) | 📖 マニュアル参照のみ |
| TEMP | official | [official/TEMP.md](official/TEMP.md) | 📖 マニュアル参照のみ |
| TIMER.X | official | [official/TIMER.X.md](official/TIMER.X.md) | 📖 マニュアル参照のみ |
| USKCGM.X | official | [official/USKCGM.X.md](official/USKCGM.X.md) | 📖 マニュアル参照のみ |
| CONFIG.SYS | official | [official/CONFIG.SYS.md](official/CONFIG.SYS.md) | 📖 マニュアル参照のみ |
| DB.X | official | [official/DB.X.md](official/DB.X.md) | ✅ インストール・起動/終了・主要コマンド約30種をix68k動作確認済み（XC付属で本体非同梱） |
| SUSIE.X | freeware | [freeware/SUSIE.X.md](freeware/SUSIE.X.md) | 📖 マニュアル参照のみ |
| SXWIN.X | official | [official/SXWIN.X.md](official/SXWIN.X.md) | ✅ FSX.X常駐→SXWIN.X起動・マウス操作（個別アプリは未検証） |
| FSX.X | official | [official/SXWIN.X.md](official/SXWIN.X.md) | ✅ SXWIN.Xの前に常駐が必要（同ファイルに記載） |
| LX.R | freeware | [freeware/LX.R.md](freeware/LX.R.md) | ✅ 起動/一覧表示/展開（他オプションは📖） |
| ZX.R | freeware | [freeware/ZX.R.md](freeware/ZX.R.md) | ✅ 起動/一覧表示/展開（他オプションは📖） |
| MINT.X | freeware | [freeware/MINT.X.md](freeware/MINT.X.md) | ✅ インストール・起動確認済み（2画面ファイラUI表示）。基本操作は📖 |
| ZMUSIC.X | freeware | [freeware/ZMUSIC.X.md](freeware/ZMUSIC.X.md) | ✅ インストール・2.08eパッチ適用・起動確認済み |
| PCM8A.X | freeware | [freeware/PCM8A.X.md](freeware/PCM8A.X.md) | ✅ インストール・AUTOEXEC.BAT登録・起動時常駐を確認済み（スイッチは📖） |
| MMDSP.R | freeware | [freeware/MMDSP.R.md](freeware/MMDSP.R.md) | ✅ インストール・patchlevel 2適用・起動/終了・ZDF演奏確認済み（オプションは📖） |
| BUP.X | freeware | [freeware/BUP.X.md](freeware/BUP.X.md) | ✅ インストール・パッチ適用動作確認済み |
| ZP.R | freeware | [freeware/ZP.R.md](freeware/ZP.R.md) | 📖 マニュアル参照のみ（インストール済み・起動未検証） |
| MZP.X | freeware | [freeware/MZP.X.md](freeware/MZP.X.md) | 📖 マニュアル参照のみ（インストール済み・起動未検証） |
| LZZ.R | freeware | [freeware/LZZ.R.md](freeware/LZZ.R.md) | ✅ インストール・BUP.Xでv0.57aへパッチ適用・ZDF展開（MMDSP経由）確認済み |
| HAPIC.X | freeware | [freeware/HAPIC.X.md](freeware/HAPIC.X.md) | 📖 マニュアル参照のみ（インストール済み・起動未検証） |
| TX.R | freeware | [freeware/TX.R.md](freeware/TX.R.md) | ❌ インストール済み・起動未検証 |
| HAS.X | freeware | [freeware/HAS.X.md](freeware/HAS.X.md) | ✅ 起動・アセンブル確認済み（has060.xをリネーム導入） |
| HLK.R | freeware | [freeware/HLK.R.md](freeware/HLK.R.md) | ✅ 起動・リンク確認済み |
| GCC.X | freeware | [freeware/GCC.X.md](freeware/GCC.X.md) | ✅ コンパイル・リンク・実行をix68k確認済み（GCC_NO_XCLIB発見） |
| LIBC | freeware | [freeware/LIBC.md](freeware/LIBC.md) | ✅ GCCからの実リンク・実行確認済み（libgnu.a/libgcc.a補完済み） |
| APICG.r | freeware | [freeware/APICG.r.md](freeware/APICG.r.md) | ✅ PICLOAD()でPICファイルロード・ix68k表示を確認済み（ライブラリ用途のみ導入） |
| TwentyOne.x | freeware | [freeware/TWENTYONE.X.md](freeware/TWENTYONE.X.md) | ✅ インストール・CONFIG.SYS常駐・21文字ファイル名識別・DELワイルドカード一括削除をix68k確認済み |
| EM.X | freeware | [freeware/EM.X.md](freeware/EM.X.md) | ✅ インストール（MicroEMACS+しゃれまっくす(SharlEMACS)差分）・PATH経由起動/終了をix68k確認済み |
| GED.X | freeware | [freeware/GED.X.md](freeware/GED.X.md) | ✅ MCP経由でロード/l/g/i/a/d/w/q/q!/?すべてix68k確認済み（GhostED） |
| HIOCS.X | freeware | [freeware/HIOCS.X.md](freeware/HIOCS.X.md) | ✅ CONFIG.SYS常駐（IOCS.Xから置き換え）・起動・フォントファイル切替を確認済み。スイッチ個別検証は未着手 |
| 2HDSIM.X | freeware | [freeware/2HDSIM.X.md](freeware/2HDSIM.X.md) | ✅ インストール・BUP.Xパッチ適用・バージョンバナー確認済み。イメージ登録・アクセスは未検証 |
| MKIMG.X | freeware | [freeware/2HDSIM.X.md](freeware/2HDSIM.X.md) | ✅ インストール・起動確認済み（バックアップ/リストア/空きディスク作成は未検証） |
| EJECT.X | freeware | [freeware/2HDSIM.X.md](freeware/2HDSIM.X.md) | ✅ インストール・起動/エラー応答確認済み |
| 060cache.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060dmam.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060dmap.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060high.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060loadhigh.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060mat.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060mmu.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | ✅ `\BIN\060turbo` へ導入・PATH登録、ix68kで起動・MMU表示を確認済み（出力例とは差異あり） |
| 060opmp.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060pcr5.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060prni.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060prnp.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060ps.X | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| 060simck.sys | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |
| exx.X（ebc/edc/eic/esb/ess.X） | freeware | [freeware/060TURBO.md](freeware/060TURBO.md) | 📖 マニュアル参照のみ（`\BIN\060turbo` へ導入・PATH登録済み、起動未検証） |

---

## Human68k 付属コマンド 未着手リスト（HUMAN302.XDF 由来）

> ファイル化する際は `official/` に置き、上の逆引きインデックスに追加します。

### 外部コマンド（.X）

| コマンド | 機能(暫定) |
|---------|-----------|
| DISKCP1.X / HDISKCP2.X / HCOPY22.X | ディスクコピー |
| CMD.X | ファイル/システム |
| COPY21.X | コピー/移動 |
| HDID.X | ドライブ |
| HISTORY.X / LD.X | ユーティリティ |
| IOCS.X | システム |
| PMDRV3.X | その他 |
| FLOAT2.X / FLOAT3.X / FLOAT4.X | 浮動小数点パッケージ |
| BASIC.X | X-BASIC |
| HFORMAT2.X / HSPEED2.X / HSWITCH2.X / SWITCH1.X / SPEED1.X / FORMAT1.X | 機種別/旧版 |

### ドライバ（.SYS）/ ファンクション（.FNC）/ バッチ（.BAT）

| ファイル | 種別 |
|---------|------|
| ASK68K.SYS / BEEP.SYS / KEY.SYS / RAMDISK.SYS / RSDRV.SYS / USKCG.SYS / PRNDRV*.SYS | デバイスドライバ |
| AUDIO.FNC / GRAPH.FNC / IMAGE.FNC / MOUSE.FNC / MUSIC.FNC / MUSIC3.FNC / SPRITE.FNC / STICK.FNC | X-BASIC 関数 |
| CONFIG.SYS / INSTALL.BAT | 起動設定 |

### COMMAND.X 内部コマンド

`official/COMMAND.X_builtin.md`: ドライブ変更（`<d:>`） / BREAK / CD・CHDIR / CLS / COPY / CTTY / DATE / DEL・ERASE / DIR / ECHO / EXIT / FOR / GOTO / HIS / IF / MD・MKDIR / MEMFREE / PATH / PAUSE / PROMPT / RD・RMDIR / REM / REN・RENAME / SET / SHIFT / TIME / TYPE / VER / VERIFY / VOL（第5章・第5.3節の内部コマンドを集約済み）。
