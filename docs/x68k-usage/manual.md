# ix68k マニュアル

ix68kは、SHARP製パーソナルワークステーションX68000のエミュレータです。このマニュアルは、
ビルドから起動・終了、AIエージェント（MCP）による操作、付属資料の読み方までの入口をまとめます。
詳細は各節のリンク先を参照します。

MC680x0 CPUコアは、Git submoduleとして取り込む `mc68000` を使用します。ix68kのリポジトリを
`--recurse-submodules` 付きでcloneすれば、`mc68000` も一緒に取得できます。

## 1. 使い方を選ぶ

ix68kは、次の3通りの方法で使えます。

| パターン | 操作する人 | 必要なもの |
|---|---|---|
| LLM連携なし | 人間が画面とキーボードで操作する | ix68k本体、ROM、ディスクイメージ |
| クラウドLLM | Claude CodeやCodex CLIなどのAIエージェントが、MCP経由で操作する | 上記に加え、`uv`、エージェントのアカウント |
| ローカルLLM | LM Studio上のモデルが、MCP経由で操作する | 上記に加え、`uv`、Node.js、tool use対応モデル |

まず「LLM連携なし」で起動できることを確認してから、MCP連携を追加するのが確実です。

## 2. 準備するもの

ix68kの動作には、X68000のIPLROM、CGROM、Human68kのブートディスクイメージ
（`HUMAN302.XDF`）が必要です。これらはリポジトリにもリリースZIPにも同梱されません。

### 手動で用意する

インストーラはありません。ROMとディスクイメージを手動で配置します。
入手方法とCGROMの生成は [QUICKSTART](../../QUICKSTART.md) の「ROM・ディスクイメージを用意する」を
参照します。

配置の例:

```text
rom/IPLROM.DAT
rom/CGROM.DAT
disk/HUMAN302.XDF
```

## 3. ビルドする

ソースから、C99コンパイラ、CMake、SDL3でビルドできます。macOSの例:

```bash
git clone --recurse-submodules https://github.com/TerukiYamaguchi/ix68k.git
cd ix68k
brew install cmake sdl3
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
```

LinuxとWindowsの手順、画面なしで動くヘッドレス版（`ix68k-nogui`）、テストの実行方法は
[ビルド・実行手順](../ix68k-internals/build.md) を参照します。

## 4. 起動と終了

### 設定ファイル

設定例をコピーして、ROM・CGROM・ディスクイメージのパスを環境に合わせて編集します。

```bash
cp profiles/ix68k.cfg.example ix68k.cfg
```

最低限の設定例:

```ini
rom   = rom/IPLROM.DAT
cgrom = rom/CGROM.DAT
sram  = sram.dat
fd0   = disk/HUMAN302.XDF
```

### 起動

```bash
./build/ix68k
```

カレントディレクトリの `ix68k.cfg` を自動で読み込みます。
無ければ、実行ファイルの隣、OSのユーザー領域の順に `ix68k.cfg` を探します。ユーザー領域は
macOS が `~/Library/Application Support/ix68k/`、Windows が `%APPDATA%\ix68k\`（設定とデータは
同じ場所）、Linux は設定が `~/.config/ix68k/`、データ（ROM・ディスク・SRAM）が `~/.local/share/ix68k/`
です。設定ファイルの相対パスは、実行ファイルの隣で見つけた場合はその隣、ユーザー領域で見つけた
場合はデータの置き場所を基準に解決します。
どこにも無く、引数なしで起動した場合（`.app` や `.desktop` からの起動）は、ユーザー領域に既定の
`ix68k.cfg` と `rom/`・`disk/` を作成します。IPLROM が無ければ置き場所を示すエラーで終了します。
ROMとCGROMはコマンドラインで指定することもできます（`-rom`、`-cgrom`）。ディスクイメージは
設定ファイルの `fd0` などで指定するか、起動後にメニュー「File > Open Disk Selector...」で挿入します。

指定できるオプションの一覧は `./build/ix68k --help` で表示できます。Human68kが起動し、
`A>` や `C>` のプロンプトが表示されれば成功です。

### 終了

- ウィンドウを閉じると、ix68kが終了します。
- 終了前に、実行中のプログラムが終わっていて、フロッピーやハードディスクへのアクセス中で
  ないことを確認します。アプリケーションの未保存の変更は失われます。
- NetBSDなど、ゲストOS側にシャットダウン手順があるOSは、先にゲストを終了させてから
  ix68kを終了します。
- 前面電源スイッチ相当の操作は、メニュー「Machine > Power Switch (OFF)」で行えます。
  MCPからは `power_switch` ツールを使います。
- 実行中のプログラムから抜けられないときは、メニュー「Machine > Interrupt」または
  ホストの `Pause` キーでNMIを発生させ、COMMAND.Xのコマンドラインへ戻る。

詳細は [Human68kの基本](guide/human68k_basics.md) の「終了と中断」を参照します。

### ジョイパッドを使う

USBのジョイパッドを繋ぐと、X68000のジョイスティックポートに接続されたものとして動きます。
起動ログの `Gamepad#N connected` の番号で、`Gamepad#1` が#1、`Gamepad#2` が#2になります。
1人用のゲームは#1だけを読みます。使いたいパッドが#2になっている場合は、`ix68k.cfg` の
`joy1` / `joy2` で割り当てを変えます。確認用に `tools-x68k/joyget_test/JOYGET_TEST.X` を同梱している
（ビルド済みバイナリ配布版。clone した場合は `tools-x68k/joyget_test/joyget_test.x`）。
詳しくは [ジョイパッドを使う](guide/joypad.md) を参照します。

### ディスクイメージを増やす

起動用のSCSI HDDイメージを作る手順は
[SCSI HDDインストール](mcp/provisioning/hdd_install.md) に固定手順があります。
AIエージェントに「SCSI HDDをインストールして」と依頼することもできます。

## 5. AIエージェントから操作する（MCP）

ix68kはMCPサーバを介して、AIエージェントから画面・キーボード・ディスクを操作できます。
接続設定の詳細は [MCPの接続設定](mcp/mcp_llm_setup.md) にあります。

共通の流れ:

1. ix68kを起動します。起動すると、MCP用の待ち受け先が作られる（macOS/Linuxは `/tmp/ix68k_mcp.sock`、Windowsは TCP `127.0.0.1:26800`）。
2. 使うクライアントに、`x68k-emulator` というMCPサーバを登録します。
3. 画面を見せるように依頼して、動作を確認します。

### クラウドLLM（Claude Code、Codex CLI）

Claude Codeは、リポジトリ直下に同梱の `.mcp.json` を使います。リポジトリのルートで `claude` を
起動すれば、通常は編集不要です。Codex CLIは `~/.codex/config.toml` に、`uv` で
`tools-host/mcp/ix68k_mcp_server.py` を起動する設定を追記します。パスの書き方は [MCPの接続設定](mcp/mcp_llm_setup.md) を参照します。

### ローカルLLM（LM Studio）

LM Studioでは、`x68k-emulator` に加えて、資料を読ませるための `filesystem` サーバを登録します。
`filesystem` サーバは Node.js の `npx` で動くため、先にNode.jsを導入します。

```bash
brew install node
```

Windowsでは `winget install OpenJS.NodeJS.LTS` で導入します。

`mcp.json` の登録例とモデルの選び方は [MCPの接続設定](mcp/mcp_llm_setup.md) の
LM Studioの節を参照します。LM Studioでは、tool use（function calling）に対応した
モデルを選びます。

登録後、チャットで次のように依頼すると、資料を探してから操作させられます。

```text
list_directory ツールで /path/to/ix68k/docs/x68k-usage/official の内容を確認して、ED.X の起動方法を調べてください。
その後、エミュレータ上で ED.X を起動して、画面を見せてください。
```

エージェントの運用方針は [エージェント運用ガイド](mcp/agent_playbook.md) と
[日常的な操作フロー](mcp/daily_workflows.md) にあります。MCPツールの引数と応答形式の正本は
`tools-host/mcp/MCP_TOOLS.md` です。

## 6. 付属資料の使い方

`docs/x68k-usage/` には、X68000の利用ガイドとソフトウェアの操作リファレンスがあります。

| 知りたいこと | 読む場所 |
|---|---|
| X68000とHuman68kが初めて | [guide/](guide/README.md) |
| コマンドやソフトの起動・終了・操作 | [逆引きインデックス](README.md) |
| MCPでの操作、環境構築 | [mcp/](mcp/README.md) |
| X68000アプリの開発 | [development/](development/README.md) |

### 逆引きインデックスの読み方

[README.md](README.md) の表は、実行ファイル名（`ED.X`、`SWITCH.X` など）から、
操作を説明したファイルを引くための索引です。

1. 表で実行ファイル名を探し、「ファイル」列のリンクを開きます。
2. 各ファイルの「リファレンス」の節で、ソフトの機能とコマンドラインを確認します。
3. 「ix68kでの利用」の節で、インストール手順とMCPでの操作を確認します。
4. 「検証状態」の列で、ix68kでの動作確認の有無を確認します。

### ビルド済みバイナリで試す場合と、cloneして使う場合

- **ビルド済みバイナリで手軽に試す**（GitHub Releasesでの配布は準備中）: アーカイブを展開し、
  同梱の `README.md` の手順で [準備するもの](#2-準備するもの) を用意して起動します。アーカイブには
  本体のほか、`docs/x68k-usage/`（本資料）、`tools-host/`（MCPサーバ、`hu68disk`、`sram_switch`、
  `aiq-bridge` など）、`tools-x68k/`（`HOSTDRV.SYS`、`GED.X` など）が入っています。
  `aiq-bridge` はmacOS / Linux版だけに入っています（`hu68disk`・`sram_switch` はWindows版にも入っています）。
- **git cloneして使う**: 手元の `docs/x68k-usage/` をそのまま参照できます。AIエージェントには
  リポジトリのルートで作業させると、逆引きインデックスと各資料を直接読ませられます。
