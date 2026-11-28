# GitHub Releases バイナリパッケージの内容

対象: Issue #195。これは `v*` タグから生成する各OS向けZIPの内容の正本であり、
Release workflow・CMakeの `install()` 規則・配布用READMEはこのファイルと一致させる。
リポジトリ内の配布設定例は `profiles/`、この文書と将来のステージング定義は
`packaging/release/` に置く。

> 状態: **手元配布**。Release workflow は作らず（GitHub Actions の無料枠を消費するため、Issue #268）、
> 各OSのZIPをメンテナの手元で作って配布する。ここに記したファイルは、まだ
> GitHub Releasesから取得できない。ZIP生成は `stage.sh`（macOS / Linux）と
> `stage.ps1`（Windows）で行え、それぞれ本書の条件を検証する。
>
> | OS | 作り方 | PGO（`packaging/release/pgo/`） |
> | --- | --- | --- |
> | Windows x64 | `powershell -File packaging\release\stage.ps1 -Compiler clang-cl -Version <版>` | clang-cl + `ix68k-windows-x64.profdata`。`-Compiler` は明示する（省略で clang-cl が無いと、警告を出して MSVC・PGO なしになる） |
> | macOS arm64 | `packaging/release/stage.sh`（macOS で実行） | Apple clang + `ix68k-macos-arm64.profdata` |
> | macOS x64 | `packaging/release/stage.sh`（Intel Mac で実行） | 同梱プロファイル無し（PGO なし） |
> | Linux x64 | `packaging/release/stage.sh`（Linux で実行。`CC` か PATH 上の clang を使う。無ければ PGO なし。メモリが小さい環境は `X68K_BUILD_JOBS=N`） | clang + `ix68k-linux-x64.profdata` |
>
> `local/profiles/` があれば、ビルド前にプロファイルを採り直す（`X68K_PGO_TRAIN=0` で省略）。
> 更新したら `packaging/release/pgo/README.md` の採取情報もコミットする。
> ZIP の起動確認も各OSで手元で行う。
>
> Windows版は `SDL3.dll` に加えて
> `VCRUNTIME140.dll` 等のVCランタイムDLLをアプリローカルで同梱する（vcpkg版SDL3.dllも動的CRTに依存するため`/MT`では解消しない）。
> Linux版は `X68K_RELEASE_LAYOUT=ON` のinstallで実行ファイルをトップ、SDL3を `lib/`（RUNPATH `$ORIGIN/lib`）へ置く。

## 共通構成

各ZIPは展開先のトップレベルをアプリケーションディレクトリとし、次を含める。

| パス | 内容 | 同梱理由 |
| --- | --- | --- |
| `ix68k.exe`（Windows）/ `ix68k`（Linux）/ `ix68k.app`（macOS） | ix68k本体。macOSはSDL3動的ライブラリ・`gamecontrollerdb.txt`を内包した`.app`バンドル | エミュレータを起動するため |
| `tools-host/ix68k-hostfile/` | `ix68k-hostfile`（Python 3のスクリプト。標準ライブラリのみで、同梱の `tools-host/mcp/ix68k_wire.py` を読む）と `README.md`。ホストとHuman68k間で単一ファイルを転送するCLI | HOSTFILE機能を使うため。MCPサーバ（`uv`）と同様にPython 3を前提とする |
| `tools-host/hu68disk/`、`tools-host/sram_switch/` | ビルド済みバイナリと `README.md`。ソースは入れない。`hu68disk` はmacOS / Linux / Windowsに入る（macOS / Linuxは `stage.sh` がMakefileで、Windowsは `stage.ps1` がCMakeの `hu68disk` ターゲットでビルド）。`sram_switch` も全OS（Windowsは同じく `stage.ps1` がCMakeの `sram_switch` ターゲットでビルド） | ディスクイメージの操作と `sram.dat` のメモリスイッチ設定を、ビルドなしで使うため。`hu68disk` は `mkfs --type scsi` のために、無償公開されたHuman68k 3.02由来のブートコード（`scsi_ipl_data.h`）を含む。`sram_switch` の `<strings.h>` はWindowsでは `_stricmp` / `_strnicmp` へ置き換え、コンソールをUTF-8出力にする（Issue #115）。`hu68disk` のOS差は `compat.h` / `compat.c` に集約し、Windowsは文字コード変換にWin32 APIを使う（Issue #31） |
| `tools-host/aiq-bridge/` | `aiq_bridge.py`・`docs_search.py`・`prompts/`・`README.md`（macOS / Linuxのみ。標準ライブラリだけのPythonスクリプトのまま入れる。テストと `test_aiq.sh` は入れない） | `tools-x68k/aiq/` の `AIQ.X` と対でAIQを使うため。ttyを使うためWindows版には入れない |
| `tools-x68k/hostdrv/` | `HOSTDRV.SYS`（Human68k側のHOSTDRVリモートデバイスドライバ）と `README.TXT` | `\SYS\HOSTDRV.SYS` へ配置してホスト共有ディレクトリをゲストドライブとして使うため |
| `tools-x68k/hostfile/` | `HOSTFILE.X`（`tools-x68k/hostfile/hostfile.x` をコピーし、ZIP内では大文字名にする）と `README.TXT` | `ix68k-hostfile` と対でホスト・ゲスト間の単一ファイル転送を使うため |
| `tools-x68k/joyget_test/` | `JOYGET_TEST.X`（`tools-x68k/joyget_test/joyget_test.x`。ソースは `tools-x68k/joyget_test/joyget_test.s`）と `README.TXT` | パッドがX68000のジョイスティック#1/#2のどちらに割り当たったかを確かめ、`joy1` / `joy2` を決めるため |
| `tools-x68k/ged/` | `GED.X`（`tools-x68k/ged/GED.X`）と `README.TXT` | MCPからHuman68k上のテキストファイルを編集するため |
| `tools-x68k/aiq/` | `AIQ.X`（`tools-x68k/aiq/aiq.x`、ZIP内では大文字名）と `README.TXT` | 自然言語の指示からHuman68kのコマンド列を作る。ホスト側の `aiq-bridge`（Python）は同梱しない |
| `tools-x68k/fntadr/` | `FNTADR.X`（`tools-x68k/fntadr/fntadr.x`、ZIP内では大文字名）と `README.TXT` | HIOCS等でANKフォントが差し替わったとき、実アドレスをエミュレータへ報告して文字認識を合わせるため |
| `tools-host/cgrom_gen/` | C製の `cgrom_gen`（Windowsは `cgrom_gen.exe`。macOS / LinuxはMakefile、WindowsはCMakeでビルドする）と `README.md` | ビットマップフォント（BDF）から互換CGROMを生成するための補助ツール。Pythonは要らない。インストーラは配布しない |
| `tools-host/mcp/` | `tools-host/mcp/` の `ix68k_mcp_server.py`・`ix68k_wire.py`・`pyproject.toml`・`uv.lock`・`MCP_TOOLS.md`・`text_profiles/` と `README.md`（リポジトリの `tools-host/mcp/` をそのままコピーする） | ソースをビルドせず、`uv` だけでMCPサーバ機能をClaude Code等から使うため |
| `.mcp.json` | リポジトリ直下の `.mcp.json` をコピーする。`uv run --project tools-host/mcp tools-host/mcp/ix68k_mcp_server.py` を `x68k-emulator` として登録する（展開先で `claude` を起動すれば読まれる） | MCPサーバの登録を手書きさせないため |
| `AGENTS.md`、`CLAUDE.md` | `packaging/release/dist_AGENTS.md` と `dist_CLAUDE.md`（中身は `@AGENTS.md` の1行）をコピーする。展開先で起動したAIエージェントに、`README.md`・`docs/x68k-usage/`・`tools-host/mcp/MCP_TOOLS.md` を読む順序と守ることを示す（リポジトリの `AGENTS.md` / `CLAUDE.md` は開発者向けのため使わない） | Claude Code は `CLAUDE.md`、Codex は `AGENTS.md` を起動時に自動で読むため。案内が無いと、利用者が指示しない限りエージェントは `docs/x68k-usage/` の参照ルールを読まない |
| `gamecontrollerdb.txt` | SDLゲームコントローラのマッピングDB | コントローラ入力を認識するため |
| `ix68k.cfg.example` | `profiles/ix68k.cfg.example` からコピーする利用者向け設定テンプレート | ROM・メディアの配置例と設定開始点を提供するため |
| `keyboard.cfg.example` | `profiles/keyboard.cfg.example` からコピーするキーボード設定テンプレート | ホストに合うキーボードprofileの設定開始点を提供するため |
| `README.md` | 展開後のROM配置、設定、起動の最小限の案内。詳細・参考文献・ビルドはGitHubを参照させる | ソースビルド不要の利用手順を提供するため |
| `rom/` | 利用者がIPLROMとCGROMを配置する空ディレクトリ | `ix68k.cfg.example` の既定配置先をあらかじめ用意するため |
| `disk/` | 利用者がHuman68k等のメディアを配置するディレクトリ。`BLANK.XDF`（フォーマット済みで空の2HDイメージ、ブート不可。雛形なのでコピーして使うよう配布用READMEに書く）だけを入れる | 設定・セットアップ資料の配置先を用意し、すぐ使える空きフロッピーを提供するため |
| `docs/x68k-usage/` | X68000利用・開発・MCP操作・セットアップ資料 | 利用者がX68000を利用・開発し、環境を構築できるようにするため |
| `LICENSES/` | リポジトリの `LICENSES/`（`ymfm-BSD-3-Clause.txt`、`SoftFloat-BSD-3-Clause.txt`、`XEiJ-License.txt`、`Sharp-Products-Users-Forum-License.txt`）をコピーするサードパーティライセンス全文 | ymfm・SoftFloat由来コードのBSD-3-Clause条件（バイナリ再配布時の表示）、XEiJ由来部分の「派生物に文書を添付する」条件、シャープ製ソフト由来部分の「文書の添付」条件（許諾条件 1-5）を満たすため |
| `LICENSE` | 採用したライセンス全文 | 配布条件を明示するため |

`tools-host/` と `tools-x68k/` はZIPのトップレベルに置く。ツールごとにサブディレクトリを掘り、
バイナリと説明文書を入れる。`tools-x68k/` の説明文書はゲストで読むため `README.TXT`（Shift-JIS・CRLF・末尾 `$1A`）、
`tools-host/` は `README.md` とする。正本は、リポジトリ内の実行ファイルと同じ場所（`tools-x68k/<名前>/README.TXT`、
`tools-host/ix68k-hostfile/README.md`）に置き、ステージングスクリプトがコピーする。`tools-x68k/` はHuman68k上で動くバイナリ、
`tools-host/` はホストOS上で動くツールに分ける。旧 `hostdrv/`・`samples/` は廃止した。

`README.md` は `packaging/release/dist_readme.md` を、各ステージングスクリプトがZIPトップへ
`README.md` としてコピーする（リポジトリルートのREADMEは使わない）。

`ix68k.cfg.example` と `keyboard.cfg.example` は `profiles/` の対応するファイルを、
ZIPのトップレベルへコピーする。`profiles/` 自体はZIPへ含めない。

macOSで`.app`をFinderから起動した場合、ix68kは実行ファイルの隣ではなくユーザー領域
(`~/Library/Application Support/ix68k/`)の`ix68k.cfg`・`rom/`・`disk/`を使う。ZIPトップの
`rom/`・`disk/`・`*.cfg.example`は、ZIPのトップで`ix68k.app/Contents/MacOS/ix68k`を端末から
起動する場合の配置先である。配布用READMEで両方の使い方を説明する。

`docs/x68k-usage/` は配下のMarkdown・必要な同梱資料を保ったままディレクトリごとコピーする。
ソースコードや開発専用資料を含む他の `docs/` サブディレクトリは
この配布物には含めない。そのため `docs/x68k-usage/` から `docs/x68k-usage/` の外（`docs/ix68k-internals/`・
`QUICKSTART.md`・ソースなど）へのリンクは、相対パスでなく GitHub の URL
（`https://github.com/TerukiYamaguchi/ix68k/blob/main/...`）で書く。

`rom/` は空のまま、`disk/` は `BLANK.XDF` だけを入れてZIPへ含める。`BLANK.XDF` は `packaging/release/BLANK.XDF`（`hu68disk mkfs` で作った1,261,568バイトのFAT12・0ファイル。ブートセクタはBPBのみでIPLコードを含まない）をコピーする。OEM名 `X68IPL30` は Human68k 3.02 のフォーマットに合わせた文字列で、権利と許諾は `LICENSES/Sharp-Products-Users-Forum-License.txt`（無償配布・ソース公開・文書添付）に従う。

Gitは空ディレクトリを追跡しないため、`rom/` はプレースホルダーファイルで代用せず、Release workflowまたはCMakeの配布ステージング処理で
明示的に作成する。

`tools-x68k/hostdrv/HOSTDRV.SYS` はホストOSに依存しないHuman68k用バイナリである。Gitで
管理している `tools-x68k/hostdrv/HOSTDRV.SYS`（m68k-xelf-gccでビルドした検証済みバイナリ）を、
各ZIPのこの配置先へコピーする。m68k-xelfの無いWindows機・CIでもビルドせず同梱できる。再ビルドの手順は `docs/ix68k-internals/rebuild_tracked_binaries.md` を参照する。
`hostdrv.c` を変更したときは再ビルドして `HOSTDRV.SYS` を再コミットする
（`stage.sh` はmacOSでの組み立て時に毎回再ビルドする）。配布対象は `.SYS` 本体だけであり、そのビルドに使うソース・Makefile・
クロスツールチェインは含めない。

## OSごとの追加ファイル

| ZIP | 追加するファイル | 配置・検証条件 |
| --- | --- | --- |
| `ix68k-macos-arm64.zip` | `ix68k.app/Contents/Frameworks/` にSDL3動的ライブラリ（`@rpath`参照、ad-hoc署名） | クリーン環境で起動確認する |
| `ix68k-macos-x64.zip` | arm64版と同じ | arm64版と同じレイアウト・検証条件 |
| `ix68k-windows-x64.zip` | `SDL3.dll`、およびクリーンなWindows環境でix68k起動に必要と判明したMSVCランタイムDLL | `.exe` と同じディレクトリへ置き、展開直後に起動確認する |
| `ix68k-linux-x64.zip` | ix68kが参照するSDL3動的ライブラリ一式 | `ubuntu-22.04` でパッケージ化し、実行ファイルから相対参照できる場所へ置く |

macOS/Linuxのライブラリの正確なパス・ファイル名は、Release実装時に採用するCMake
install layoutで確定する。ライブラリ探索がホストにインストール済みのSDL3へ依存しない
ことを、展開後の起動試験で確認する。

## 同梱しないもの

- IPLROM / CGROM
- Human68kのディスクイメージ、HDS/XDF等のメディア
- 開発用ソース、テスト、ビルドツールチェイン

ROMとメディアを同梱しない理由と利用者自身による入手・配置方法は、配布用READMEと
`ix68k.cfg.example` に明記する。

## `ix68k-hostfile` のOS別接続方式

| OS | 接続方式 | 複数インスタンス指定 |
| --- | --- | --- |
| macOS | AF_UNIX | `IX68K_MCP_SOCK` / `--sock` |
| Linux | AF_UNIX | `IX68K_MCP_SOCK` / `--sock` |
| Windows | TCP loopback | `IX68K_MCP_PORT` / `--port` |

WSL上のCLIからネイティブWindows版ix68kへ接続する組合せは対象外である。

## Release前の確認

- 各OSジョブでReleaseビルドとテストが成功すること。
- ステージング先をZIP化し、クリーン環境へ展開してix68kがSDL3依存を解決して起動すること。
- 各OSのZIPで `python3 tools-host/ix68k-hostfile/ix68k-hostfile --help` が成功すること（Windowsは `python`）。
- `ix68k-hostfile` のMCPワイヤ期待版が同梱ix68k本体と一致すること。
- ROM・CGROM・Human68kメディアがZIPに含まれないこと（`disk/BLANK.XDF` は例外）。
