# AIエージェントによるエミュレータ初回インストール手順（自然文1つで完結させる）

Claude Code や LM Studio 等、**ホストのシェルコマンドを実行できるAIエージェント**に対し、
次のような自然文を渡すだけで、このマシン上の別ディレクトリにエミュレータを新規インストールし、
起動可能な状態にするための固定手順（レシピ）。

> `~/x68000_test` にエミュレータを新規インストールして、起動できるところまでやって

`docs/x68k-usage/mcp/provisioning/hdd_install.md`（SCSI HDDインストール手順）と同様、**この手順はAIエージェントが
読んで実行することを想定している**（都度LLMが手順を考案するのではなく、毎回同じ手順を辿らせて
再現性を確保するのが目的）。人間が手動で行う場合もそのまま使えます。

配布マニュアルのうち「エミュレータのビルド手順」「起動・終了方法」「ディスクイメージの設定方法」
に対応する部分。LM Studio + MCP 経由の操作方法・`docs/x68k-usage/` の使い方は
[manual.md](../../manual.md) を参照。

> **インストーラは配布していません**: ビルド済み配布物の導入は、展開した`README.md`の手順（手動配置）に従います。
> 本手順は、手元のビルド成果物（`build/ix68k`）を別ディレクトリへ導入する場合や、各ステップを
> 個別に再現する場合に使います。

---

## 将来との違い（重要）

**本番想定（GitHub Releases整備後）は「Releasesからビルド済みアーカイブをダウンロードして展開する」
だけで完結する**（[QUICKSTART](../../../../QUICKSTART.md)参照）。しかしGitHub Releasesでの
ビルド済みバイナリ配布は現在準備中（未公開）のため、本手順では代わりに**このリポジトリの作業ツリーから
必要なファイルを直接コピーする**ことでビルド済み配布物を再現します。配布開始後は Step 2〜3 が
「Releasesからダウンロード→展開」に置き換わり、それ以外（ROM入手・設定・起動）は変わらない想定。

---

## 前提

| 項目 | 確認方法 |
|------|---------|
| このマシンでビルド済み（`build/ix68k`等） | 無ければ[ビルド・実行手順](../../../ix68k-internals/build.md)に従って先にビルドする |
| `curl` / `unar`（LZH展開）/ `unzip` が使える | `unar -v` / `unzip -v` |
| retropc.net への接続 | httpsは証明書の検証に失敗するため、URLは`http://retropc.net/...`を使う（Step 4-1・4-3のコマンド参照）。ftp.iij.ad.jp等それ以外の接続先でも`curl`は使わない |

---

## Step 1: ユーザーに質問する

**1問ずつ**、以下の文言で聞く（複数まとめて聞かない）。

### Q1. インストール先ディレクトリ

> エミュレータをインストールするディレクトリの絶対パスを教えてください（例:
> `~/x68000_test`。存在しなければ新規作成します。既存ディレクトリの場合、中身を
> 上書きしてよいか後ほど確認します）。

- 既存ディレクトリで`ix68k`（または`ix68k.exe`）が既にある場合、上書きしてよいか確認します。

### Q2（おまけ）. ホストキーボード種別

> お使いのホストキーボードを選んでください。

1. MacBook US（`macbook_us`）
2. MacBook JP（`macbook_jp`）
3. テンキー付きMac US（`mac_us_keypad`）
4. テンキー付きMac JP（`mac_jp_keypad`）
5. PC US・テンキーなし（`pc_us`）
6. PC JP・テンキーなし（`pc_jp`）
7. PC US・テンキーあり（`pc_us_keypad`）
8. PC JP・テンキーあり（`pc_jp_keypad`）

> わからなければ、配列だけ`jis`/`us`で回答してください。

- 未回答・「わからない」等なら、OSに応じたJP・テンキーなしprofileを採用します。
- Step 5とは別に、回答に対応するprofileを`keyboard.cfg`へ設定します。

### Q3. CGROM.DAT（または互換データ）をお持ちですか？

> CGROM（キャラクタジェネレータROM）データを既にお持ちですか？（実機から吸い出した
> `CGROM.DAT`、他エミュレータの`CGROM_XEiJ.DAT`等、786432バイト(`0xC0000`)のファイル）
> お持ちでなければ、フリーの東雲フォントから互換データをその場で生成します。

- 「はい」等ならQ3-2へ進みます。
- 「いいえ」「持っていない」「わからない」等（既定）なら、Step 4-2で
  `tools-host/cgrom_gen/cgrom_gen.py`により東雲フォントから生成します。

#### Q3-2（Q3で「はい」の場合のみ）. そのファイルの絶対パスを教えてください

> CGROM互換データの絶対パスを教えてください。

- ファイルサイズが786432バイトでなければ、正しいファイルか確認を促す
  （`cgrom_gen.py`が出力するCGROMも実機の`CGROM.DAT`も必ずこのサイズになる）。

---

## Step 2（このマシン限定・将来はGitHub Releasesからの取得に置き換わる）: ビルド済みバイナリを確認する

```sh
ls build/ix68k build/gamecontrollerdb.txt 2>/dev/null   # macOS/Linux
ls build_win/ix68k.exe 2>/dev/null                        # Windows
```

無ければ[ビルド・実行手順](../../../ix68k-internals/build.md)に従ってビルドしてから次に進みます。

`docs/x68k-usage/mcp/provisioning/hdd_install.md`（SCSI HDDインストール）で使う`hu68disk`・`sram_switch`も
このマシン限定のホスト側CLIツール（CMakeとは別の独立Makefile）。無ければビルドする:

```sh
ls tools-host/hu68disk/hu68disk tools-host/sram_switch/sram_switch 2>/dev/null
[ -f tools-host/hu68disk/hu68disk ] || make -C tools-host/hu68disk
[ -f tools-host/sram_switch/sram_switch ] || make -C tools-host/sram_switch
```

## Step 3: インストール先ディレクトリへ必要なファイルをコピーする

```sh
mkdir -p <Q1のディレクトリ>/rom <Q1のディレクトリ>/disk

# 実行ファイル（プラットフォームに応じて選択）
cp build/ix68k <Q1のディレクトリ>/                  # macOS/Linux
# cp build_win/ix68k.exe <Q1のディレクトリ>/         # Windows

# ゲームコントローラDB（CMakeがbuild/へ自動コピー済みのものをそのまま持っていく。
# 無くても起動はするがジョイスティック割り当てが効かない）
cp build/gamecontrollerdb.txt <Q1のディレクトリ>/

# （任意）MCP経由でmacOS↔Human68k間ファイル転送を使う場合の68k側バイナリ。
# ここではホスト側に置くだけ（ゲストディスクへの実際の組み込みは別途、
# hu68disk copy 等で行う。本手順のスコープ外）
cp tools-x68k/hostfile/hostfile.x <Q1のディレクトリ>/

# hu68disk・sram_switch（ホスト側CLIツール。docs/x68k-usage/mcp/provisioning/hdd_install.mdで使う）
cp tools-host/hu68disk/hu68disk <Q1のディレクトリ>/
cp tools-host/sram_switch/sram_switch <Q1のディレクトリ>/
```

## Step 4: ROM・Human68kブートディスクを取得する

IPLROM・HUMAN302.XDFは著作権者（シャープ／STUDIO KAMADA・鎌田誠氏）がretropc.netで
無償公開しているものをダウンロードします。CGROMは著作権上フリー配布されていないため、
Q3の回答に応じて「手持ちのファイルをコピー」または「`tools-host/cgrom_gen/cgrom_gen.py`で
東雲フォントから生成」のいずれかで用意する（詳細は4-2参照）。

### 4-1. IPLROM（retropc.net、動作検証済み・MD5一致確認済み 2026-08-14）

```sh
mkdir -p arc/retropc.net/x68000/software/sharp/x68bios
[ -f arc/retropc.net/x68000/software/sharp/x68bios/X68BIOSX.LZH ] || \
  curl -L -o arc/retropc.net/x68000/software/sharp/x68bios/X68BIOSX.LZH \
    "http://retropc.net/x68000/software/sharp/x68bios/X68BIOSX.LZH"

mkdir -p /tmp/x68biosx_extract && cd /tmp/x68biosx_extract && \
  unar -f -D arc/retropc.net/x68000/software/sharp/x68bios/X68BIOSX.LZH
cp /tmp/x68biosx_extract/IPLROMXV.DAT <Q1のディレクトリ>/rom/
```

### 4-2. CGROM

Q3で回答が分岐します。いずれの経路でも最終的に`<Q1のディレクトリ>/rom/CGROM.DAT`
（786432バイト）を用意し、Step 5の`ix68k.cfg`では`cgrom = rom/CGROM.DAT`と記述します。

#### Q3で「はい」の場合: 手持ちのファイルをコピーする

```sh
cp <Q3-2で聞いたパス> <Q1のディレクトリ>/rom/CGROM.DAT
```

#### Q3で「いいえ」の場合: 東雲フォントから生成する（動作検証済み 2026-08-14）

`tools-host/cgrom_gen/cgrom_gen.py`はゴシック体・明朝体フォント（TTF/OTF/TTC）から実機互換の
CGROMを合成するクリーンルーム実装ツール（*Inside X68000* p.218-226のCGROMアドレス配置仕様に基づきます。
他エミュレータ/ツールのソースは参照・移植していない）。
フォント自体は著作権の都合上どこにも同梱せず、都度ユーザー手元のフォントを指定します。

OS標準フォント（ヒラギノ角ゴ/明朝、MS ゴシック/明朝等、`--gothic`/`--mincho`省略時に
自動検出される）はアウトラインフォントのため、CGROMのような極小サイズに縮小すると
輪郭がぶれて読みにくい。**東雲フォント**（パブリックドメイン、16x16専用設計）を使うと
実機に近い読みやすさになることを比較検証済み
（`tools-host/cgrom_gen/README.md`「推奨フォント」節参照）。

```sh
mkdir -p arc/ftp.iij.ad.jp/pub/osdn.jp/users/8/8542
[ -f arc/ftp.iij.ad.jp/pub/osdn.jp/users/8/8542/jfdotfont-20150527.zip ] || \
  curl -L -o arc/ftp.iij.ad.jp/pub/osdn.jp/users/8/8542/jfdotfont-20150527.zip \
    "https://ftp.iij.ad.jp/pub/osdn.jp/users/8/8542/jfdotfont-20150527.zip"

mkdir -p /tmp/shinonome_extract
unzip -o -j arc/ftp.iij.ad.jp/pub/osdn.jp/users/8/8542/jfdotfont-20150527.zip \
  "JF-Dot-Shinonome16.ttf" "JF-Dot-ShinonomeMin16.ttf" -d /tmp/shinonome_extract

python3 tools-host/cgrom_gen/cgrom_gen.py \
  --gothic /tmp/shinonome_extract/JF-Dot-Shinonome16.ttf \
  --mincho /tmp/shinonome_extract/JF-Dot-ShinonomeMin16.ttf \
  --output <Q1のディレクトリ>/rom/CGROM.DAT
```

生成に数秒で完了します。出力は必ず786432バイト（`0xC0000`）になる（`assert`で検証済み）。
自動検出フォント（`--gothic`/`--mincho`省略）を使いたい場合は`tools-host/cgrom_gen/README.md`
の「フォントの自動検出」節を参照。

### 4-3. Human68k v3.02 ブートディスク（retropc.net、動作検証済み・MD5一致確認済み 2026-08-14）

```sh
mkdir -p arc/retropc.net/x68000/software/sharp/human302
[ -f arc/retropc.net/x68000/software/sharp/human302/HUMN302I.LZH ] || \
  curl -L -o arc/retropc.net/x68000/software/sharp/human302/HUMN302I.LZH \
    "http://retropc.net/x68000/software/sharp/human302/HUMN302I.LZH"

mkdir -p /tmp/human302_extract && cd /tmp/human302_extract && \
  unar -f -D arc/retropc.net/x68000/software/sharp/human302/HUMN302I.LZH
cp /tmp/human302_extract/HUMAN302.XDF <Q1のディレクトリ>/disk/
```

いずれのLZHにも許諾条件（ライセンス）テキストが同梱されています。有償配布物（書籍・雑誌の
付録CD-ROM等）への収録は許諾条件で禁止されているが、個人利用・本手順のような無償配布の
範囲では問題ない（CGROMはフォントもツールもリポジトリ配布物なので別途この制約の対象外）。

## Step 5: `ix68k.cfg`を新規作成する

個人環境の`ix68k.cfg`（`hostdrv_dir`の絶対パス等）をそのままコピーしないこと。以下の内容で
インストール先に新規作成する（`sram.dat`は初回起動時にROM内蔵デフォルトから自動生成されるため、
事前に用意する必要はない）:

```
rom   = rom/IPLROMXV.DAT
cgrom = rom/CGROM.DAT
sram  = sram.dat
fd0   = disk/HUMAN302.XDF
series = xvi
keyboard_layout = jis
```

- `keyboard_layout`は、X68000側に見せるキー配列（`jis`/`us`）です。Q2の回答が`us`系のときだけ`us`にします。
- 下の`keyboard.cfg`の`profile`は、ホスト予約キー（F11・Pause・ディスクウィンドウ等のホットキー）の割り当てです。
  配列とは別の設定なので、どちらもQ2の回答に合わせて設定します。

> **配置の違い**: 本手順は、`<Q1のディレクトリ>`に`ix68k`（素のバイナリ）・
> `ix68k.cfg`・`rom/`・`disk/`を並べ、そこで`./ix68k`を実行する構成です。macOSで`ix68k.app`を
> `/Applications`などへ置く場合は、`ix68k.cfg`・`rom/`・`disk/`を
> `~/Library/Application Support/ix68k/`へ置きます。`ix68k.app`をFinderから起動した場合は、
> この`<Q1のディレクトリ>`の設定は使われません。

### ホスト予約キー

キーボード設定は`ix68k.cfg`から分離した`keyboard.cfg`で管理します。リポジトリの
`profiles/keyboard.cfg.example`を実行ディレクトリの`keyboard.cfg`としてコピーし、ホストに合うprofileを選びます。用意済みのprofileは
`macbook_us`、`macbook_jp`、`mac_us_keypad`、`mac_jp_keypad`、`pc_us`、`pc_jp`、
`pc_us_keypad`、`pc_jp_keypad`です。

`keyboard.cfg`は`ix68k.cfg`と同じ順（カレントディレクトリ、実行ファイルの隣、OSのユーザー
領域。Linuxは`~/.config/ix68k/`）で探します。`.app`や`.desktop`から起動する場合はユーザー領域に置きます。

profile既定値を個別に変える場合、値は`f1`〜`f24`に任意で`shift`、`ctrl`、`alt`、
`gui`（macOSではCommand）を`+`で連結した形、または無効化する`none`で指定します。
F11（フルスクリーン）とPause（NMI）は予約済みのため指定しません。

```ini
# 例: MacBook JP
profile = macbook_jp

# profile既定値を上書きする例
disk_window_hotkey = f12
software_keyboard_hotkey = shift+f12
xf1_hotkey = gui+f1
kigou = ctrl+f6
touroku = ctrl+f7
break = ctrl+f8
```

F13〜F17は全OSでXF1〜XF5へ直接対応します。テンキー、Insert、Caps Lock、Copy、Help、
Undoの専用キーも、対応するキーを備えたホストキーボードでは自動的にX68000へ送られます。

## Step 6: エミュレータを起動する

```sh
cd <Q1のディレクトリ> && ./ix68k          # macOS/Linux
# cd <Q1のディレクトリ> && ix68k.exe    # Windows
```

MCPサーバ経由で操作可能な場合は`screenshot`ツール等でHuman68k起動バナー～`Command version 3.00`～
`A>`プロンプトまで到達していることを確認します。MCP接続が無い場合はユーザーに画面を目視確認して
もらいます。

## Step 7: 結果をユーザーに報告する

インストール先ディレクトリ・コピーしたファイル一覧・ROM/ディスクの入手元・
`ix68k.cfg`の内容・起動確認結果（画面に何が表示されたか）をまとめて報告します。

---

## 注意事項

- `docs/x68k-usage/mcp/provisioning/hdd_install.md`（SCSI HDDから起動する構成にする手順）は本手順の後に続けて
  実施できます。フロッピー起動よりシステム起動・日本語入力が高速になります。同docs内のコマンド例は
  リポジトリまたはリリースZIPの展開先（`tools-host/hu68disk/hu68disk`等）での実行を前提に書かれている
  ため、Step 3でコピーした`<Q1のディレクトリ>/hu68disk`・`<Q1のディレクトリ>/sram_switch`を使う場合は
  パスを読み替える（`<Q1のディレクトリ>`に`cd`した上で`./hu68disk`・`./sram_switch`、
  ディスクイメージは`disk/<ファイル名>`とする。詳細は同docsの「コマンドのパスについて」参照）。
- ROM・HUMAN302.XDFは著作権上リポジトリに同梱されていない（[QUICKSTART](../../../../QUICKSTART.md)参照）。本手順は
  それらを都度取得する前提。
- CGROMは既存ファイルを持っていなければ`tools-host/cgrom_gen/cgrom_gen.py`（東雲フォント使用）で
  その場生成する（フォントはリポジトリに同梱しない）。詳細・トラブルシュートは
  `tools-host/cgrom_gen/README.md`参照。
