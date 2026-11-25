# ix68k

SHARP製パーソナルワークステーションX68000のエミュレータです。このアーカイブは
ビルド済みバイナリ配布版です。この文書は、展開したファイルを自分で配置して導入する手順です（インストーラはありません）。
機能・ソース・Issue・参考文献などの詳細は、GitHubを参照してください。

- <https://github.com/TerukiYamaguchi/ix68k>

## 同梱物

| パス | 内容 |
| --- | --- |
| `ix68k.exe`（Windows）/ `ix68k`（Linux）/ `ix68k.app`（macOS） | エミュレータ本体（SDL3同梱） |
| `ix68k.cfg.example`、`keyboard.cfg.example` | 設定ファイルのひな形 |
| `rom/` | IPLROMとCGROMを置くディレクトリ（Windows / Linux。macOSでは使いません） |
| `disk/` | ディスクイメージを置くディレクトリ（Windows / Linux。macOSでは使いません）。`BLANK.XDF`（フォーマット済みで空の2HDフロッピー、ブート不可。雛形なので、コピーして使ってください）が入っています |
| `.mcp.json` | Claude Code用のMCPサーバ登録（「AIエージェントから操作する」を参照。隠しファイルです） |
| `tools-host/` | ホスト側のツール。`cgrom_gen`（互換CGROMの生成）、`ix68k-hostfile`（ファイル転送）、`mcp/`（MCPサーバ）、`hu68disk`（ディスクイメージの操作）、`sram_switch`（`sram.dat`の設定）、`aiq-bridge`（AIQ.Xのホスト側）など。ツールごとに説明（`README.md`）があります。`aiq-bridge`はmacOS / Linux版だけに入っています（`hu68disk`・`sram_switch`は全OSに入っています） |
| `tools-x68k/` | Human68k側のツール（`HOSTDRV.SYS`、`GED.X`など）。ツールごとに説明（`README.TXT`。Human68k上で読めるようShift-JISです）があります |
| `docs/x68k-usage/` | 利用ガイド・ソフトウェア操作リファレンス |
| `LICENSES/` | 同梱するサードパーティのライセンス全文 |

IPLROM、CGROM、Human68kのディスクイメージは著作権の都合で同梱していません。

## 1. 導入する

インストーラはありません。ZIPを展開し、IPLROM・CGROM・Human68kのディスクイメージと設定ファイルを
手で配置します。

### ROMとディスクイメージを用意する

| 必要なもの | 入手方法 |
| --- | --- |
| IPLROM（`IPLROM.DAT`） | 実機から吸い出す（[XEiJ: IPLROMの作り方](https://stdkmd.net/xeij/iplrom16.htm)）。または[retropc.net](http://retropc.net/x68000/software/)の`X68BIOSX.LZH`を展開して使う |
| Human68k（`HUMAN302.XDF`） | [retropc.net](http://retropc.net/x68000/software/)の`HUMN302I.LZH`を展開して使う |
| CGROM（`CGROM.DAT`） | 実機から吸い出す。または`tools-host/cgrom_gen/ix68k-cgrom-gen`で互換CGROMを生成する（下記） |

LZHの展開には、7-Zipや`unar`などを使います。

互換CGROMは、パブリックドメインの[東雲フォント](http://jikasei.me/font/jf-dotfont/)
（`jfdotfont-20150527.zip`、約40MB）などから生成できます。ZIPを展開し、中のフォントファイルを
指定します。PythonやPillowは要りません。詳しくは`tools-host/cgrom_gen/README.md`を参照してください。

```sh
# Windows は ix68k-cgrom-gen.exe
tools-host/cgrom_gen/ix68k-cgrom-gen \
  --gothic "<ゴシック体フォントのパス>" --mincho "<明朝体フォントのパス>" \
  --output rom/CGROM.DAT
```

`--gothic`と`--mincho`を省略すると、OS標準のフォントで生成します（文字が粗くなります）。

### 配置する

`ix68k.cfg.example`を`ix68k.cfg`としてコピーし、次のように置きます
（Human68kのディスクイメージは`HUMAN302.XDF`など）。

```text
rom/IPLROM.DAT
rom/CGROM.DAT
disk/HUMAN302.XDF
```

```ini
rom   = rom/IPLROM.DAT
cgrom = rom/CGROM.DAT
fd0   = disk/HUMAN302.XDF
```

`ix68k.cfg`は、Windows / Linuxでは実行ファイルと同じ場所に置きます。macOSでは
`~/Library/Application Support/ix68k/`に置きます（端末から起動するときは、カレントディレクトリでもかまいません）。
`rom/`・`disk/`は`ix68k.cfg`と同じ場所に置きます。macOSでは、`ix68k.app`を`/Applications`などへ
コピーしておくと、LaunchpadとFinderから開けます（展開先の`rom/`・`disk/`は使いません）。
詳しくは[GitHubのQUICKSTART](https://github.com/TerukiYamaguchi/ix68k/blob/main/QUICKSTART.md)を
参照してください。

## 2. 起動する

- **Windows**: `ix68k.exe`を実行します。
- **Linux**: `./ix68k`を実行します。WSL2（WSLg）で起動するときは、音を出すために`libpulse0`が必要です
  （`sudo apt install -y libpulse0`）。入っていないと、`Audio output unavailable`と出て音なしで動きます。
  ディスクウィンドウの SELECT（ファイル選択ダイアログ）には、`zenity`または`xdg-desktop-portal`が必要です
  （`sudo apt install -y zenity`）。WSL2や最小構成のLinuxには入っていないことが多く、無いとSELECTを押しても
  何も起きません（ログに`file dialog: 開けませんでした`と出ます）。入れられないときは、`ix68k.cfg`の`fd0`などで
  ディスクを指定してください。
- **macOS**: LaunchpadまたはFinderから`ix68k`を開きます。`~/Library/Application Support/ix68k/`の
  `ix68k.cfg`・`rom/`・`disk/`を使います。端末からは`ix68k.app/Contents/MacOS/ix68k`でも起動できます。

ダウンロードした`ix68k.app`は未公証のため、初回起動時にmacOSが警告を出すことがあります。
その場合は、Finderで右クリックして「開く」を選ぶか、システム設定の「プライバシーとセキュリティ」で
許可してください。

## 3. バージョンアップ

- **macOS**: 新しいZIPを展開し、`ix68k.app`を`/Applications`へ置き換えます。ix68kを起動しているときは、
  終了してから行ってください。`~/Library/Application Support/ix68k/`の設定・ROM・ディスク・SRAMは
  変更されません。新しい設定項目は、新しいZIPの`ix68k.cfg.example`を見て、`ix68k.cfg`に必要なものを足してください。
- **Windows / Linux**: 新しいZIPを別のフォルダへ展開し、`ix68k.cfg`、`rom/`、`sram.dat`、`disk/`に
  置いた自分のディスクイメージをコピーします。同じフォルダへ上書き展開すると、`disk/BLANK.XDF`や
  `docs/`、`tools-*/`は新しいもので置き換わります。

## AIエージェントから操作する（任意）

導入は上の手順で完了しています。さらに、Claude Codeなどから、ビルドせずにix68kを操作することもできます（MCP）。`uv`を導入し、ix68kを起動してから、
展開先で`claude`を起動してください（展開先の`.mcp.json`が読み込まれます）。
詳しくは`tools-host/mcp/README.md`を参照してください。

## 資料

- [ix68kマニュアル](docs/x68k-usage/manual.md)
- [利用ガイド・ソフトウェア操作リファレンス](docs/x68k-usage/README.md)
- 機能の詳細・参考文献・ソースからのビルド: [GitHub](https://github.com/TerukiYamaguchi/ix68k)

## ライセンス

FM音源コアはBSD 3-Clause Licenseの[ymfm](https://github.com/aaronsgiles/ymfm)（Copyright (c) 2021, Aaron Giles）
に由来します。ライセンス全文は`LICENSES/`にあります。
