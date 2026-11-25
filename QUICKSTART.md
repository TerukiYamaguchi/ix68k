# ix68k クイックスタート

ix68kを初めて起動するまでの手順です。ix68kの概要は [README.md](README.md) を参照してください。

## AIエージェントに導入を任せる

お使いのAIエージェントに「README.mdを読んでix68kをインストールして」と言うだけで
インストールできます（多分）。

エミュレータの導入と起動用SCSI HDDの作成に使う固定手順は、
[README.md](README.md) の「コーディングエージェントでインストールする」を参照してください。
あわせて次の資料も使えます。

- [ADPCM／Z-MUSIC音源環境の導入](docs/x68k-usage/mcp/provisioning/sound_driver_install.md)
- [MCPの接続設定](docs/x68k-usage/mcp/mcp_llm_setup.md)

## 人間が導入する

### 導入手順

#### 1. ビルド済みバイナリを入手する

[Releases](https://github.com/TerukiYamaguchi/ix68k/releases)からお使いのOS向けの
アーカイブをダウンロードして展開します。ソースビルドは不要です。アーカイブには、エミュレータ本体
（SDL3同梱）、`ix68k.cfg.example`、空の`rom/`・`disk/`、HOSTDRV関連ファイル、利用ガイドが
入っています。同梱物の一覧は展開後の`README.md`を参照してください。
動作条件は、macOS 26以降（Apple Silicon）、Linux（x86_64、glibc 2.34以上）、Windows 10以降（x64）です。
詳しくは[README](README.md#動作条件)を参照してください。

> GitHub Releasesでのビルド済みバイナリ配布は現在作成中です。配布開始前にソースから
> 利用する場合は、[ビルド・実行手順](docs/ix68k-internals/build.md)を参照してください。

#### 2. ROM・ディスクイメージを用意する（必須）

動作にはX68000実機のIPLROMとCGROM、およびHuman68k（OS）のブートディスクイメージ
（`HUMAN302.XDF`）が必要です。IPLROMとHuman68k媒体は同梱していません。
以下の方法で用意してください。

- **実機所有者**: お手持ちのX68000実機からROMを吸い出してください。手順は
  [XEiJ: IPLROMの作り方](https://stdkmd.net/xeij/iplrom16.htm)を参照してください。
- **STUDIO KAMADA**: SHARPが公式に無償配布を許諾したIPLROM・`HUMAN302.XDF`が
  [retropc.net/x68000/software/](http://retropc.net/x68000/software/)で公開されています。
- **CGROM**: 実機から吸い出したCGROMを使うか、パブリックドメインの東雲フォントなどの
  ビットマップフォント（BDF）から[`tools-host/cgrom_gen/`](tools-host/cgrom_gen/)で
  互換CGROMを生成してください。

生成例（東雲フォントのBDFを展開したフォルダで実行します。作れるのは16x16漢字と8x16半角です）:

```sh
tools-host/cgrom_gen/cgrom_gen --k16 shnmk16.bdf --a8x16 shnm8x16r.bdf --output rom/CGROM.DAT
```

次のように配置します。

```text
rom/IPLROM.DAT
rom/CGROM.DAT
disk/HUMAN302.XDF
```

#### 3. 設定ファイルを作る

同梱の`ix68k.cfg.example`を`ix68k.cfg`という名前でコピーし、実行ファイルと同じ場所に置きます。
最低限、起動するディスクを指定します。

```ini
rom   = rom/IPLROM.DAT
cgrom = rom/CGROM.DAT
sram  = sram.dat
fd0   = disk/HUMAN302.XDF
```

`sram.dat`は用意する必要はありません。ファイルが無ければ初回起動時にROMの既定値で初期化し、
終了時に保存します（詳細は「設定ファイルとSRAM」を参照）。

`ix68k.cfg.example`には、CPU・FPU・SCSI・HOSTDRVなどの設定項目を、取り得る値つきで
コメントとして載せています。

#### 4. 起動する

- **Windows**: `ix68k.exe`を実行します。
- **Linux**: `./ix68k`を実行します。
- **macOS**: `ix68k.app`をFinderから起動します（この場合の設定・ROMの置き場所は下記「設定ファイルとSRAM」を参照）。
  端末から使うときは、展開先で`ix68k.app/Contents/MacOS/ix68k`を実行します。

ダウンロードした`ix68k.app`は未公証のため、初回起動時にmacOSが警告を出すことがあります。
その場合は、Finderで右クリックして「開く」を選ぶか、システム設定の「プライバシーとセキュリティ」で
許可してください。

## 設定ファイルとSRAM

`ix68k.cfg`と`keyboard.cfg`は、カレントディレクトリ、実行ファイルの隣、OSのユーザー領域の
順に探します。`-config`で指定した場合は探索しません。ユーザー領域は次のとおりです。

| OS | 設定（`ix68k.cfg`、`keyboard.cfg`） | データ（ROM、ディスク、SRAM） |
| --- | --- | --- |
| macOS | `~/Library/Application Support/ix68k/` | 同左 |
| Windows | `%APPDATA%\ix68k\` | 同左 |
| Linux | `~/.config/ix68k/`（`$XDG_CONFIG_HOME/ix68k/`） | `~/.local/share/ix68k/`（`$XDG_DATA_HOME/ix68k/`） |

設定ファイルの相対パス（`rom`、`fd0`、`sram`など）は、カレントや実行ファイルの隣で見つけた場合は
その設定ファイルのあるディレクトリ、ユーザー領域で見つけた場合はデータの置き場所を基準に
解決します。メニュー「File > Open Data Folder」でこのフォルダをFinder・エクスプローラー・
ファイラ（`xdg-open`）で開けます（macOSの`~/Library`は既定で隠れています）。

設定ファイルがどこにも無い状態で、引数なしで起動した場合（`.app`や`.desktop`からの起動）は、
ユーザー領域に`ix68k.cfg.example`と同じ内容の`ix68k.cfg`（設定の置き場所）と、ROM用の`rom/`・
ディスクイメージ用の`disk/`（データの置き場所）を作成します。IPLROMが`rom/`に無ければ、
置き場所を示すエラーを表示して終了します。`rom/`と`disk/`にファイルを置き、必要なら
`ix68k.cfg`の`fd0`などを編集して、再起動してください。

`sram.dat`は、X68000のSRAM内容を起動をまたいで保存するファイルです。初回起動時には実機ROMの
既定値相当で自動生成されるため、事前に用意する必要はありません。起動優先順位やメモリ容量を
変更する場合は、エミュレータを停止してから[`tools-host/sram_switch/`](tools-host/sram_switch/)を
使用してください。

## IPLROM 1.6 の作り方（XEiJ で生成）

IPLROM 1.6 は、XEiJ が起動時にメモリ上で IPLROM 1.3 へパッチを当てて作るROMです
（パッチ済みIPLROM 1.3は配布できないため、配布物はありません）。MC68060 + 060turbo.sys
の検証など、新しいROMが必要なときは、手元の XEiJ で次のように生成します。詳細は
[XEiJ: IPLROM 1.6](https://stdkmd.net/xeij/iplrom16.htm)を参照してください。

1. [XEiJ](https://stdkmd.net/xeij/)を入手します（OpenJDK の最新版が必要です）。
2. 設定を汚さないよう、一時 ini と `-saveonexit=off` を指定して起動します。
   機種は `-model` で指定します（Hybrid / 060turbo では常に IPLROM 1.6 が使われ、それ以外の機種は
   `-iplrom16=on` で選びます。上記の XEiJ のページによります。ここでは `-model=060turbo` でのみ生成を試しました）。

   ```sh
   java --enable-native-access=ALL-UNNAMED -jar XEiJ.jar -ini=xeijtemp.ini \
     -config=default -saveonexit=off -model=060turbo -iplrom16=on -memory=12
   ```

3. メニューの「設定 → ROM ファイルの作成 → XEIJIPLROM30.DAT」を選び、保存します
   （128KB。`$00FE0000-$00FFFFFF`）。
4. 保存したファイルを `rom/IPLROM16.DAT` として置き、`ix68k.cfg` の `rom =` に指定します。

注意:

- ix68k が読めるIPLROMのサイズは128KBと1MBです。`-iplrom256k=on` で作る256KB版は使えません。
- ROM のバージョンは XEiJ のリリースで変わります（2025-08-03 版の `_ROMVER` は `$16250803`）。
- 060turbo.sys は XEiJ 同梱の `misc/060tsys/060turbo.sys`（v0.61 で IPLROM 1.6 での動作を確認）を
  使ってください。v0.56 は `ROM のバージョンが違います` で登録を拒否することを確認しています
  （v0.56 は `_ROMVER` が `$15970213` / `$15970529` 以外を受け付けません）。v0.57〜v0.60 は
  試していません。
- 生成したROMや060turbo.sysは再配布しないでください。
