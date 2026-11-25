# ix68k

SHARP製パーソナルワークステーション X68000 のエミュレータです。Intelligent X68000 を目指す ix68k です。MC68000 コアには
[cmc68000](https://github.com/TerukiYamaguchi/cmc68000) を Git サブモジュールとして使用しています。

ix68kは、X68000ソフトウェアを動かすだけでなく、X68000向けソフトウェアを開発し、自動化し、問題を調査するための環境です。

## ix68kの特徴

- X68000用ソフトウェアを実行できます。
- macOS（開発に使用）、Windows、Linux（Ubuntu）で動作します。
- X68000用ソフトウェアを新規に開発するための強力なデバッグ・開発機能を搭載しています。
- 対応 CPU は 68000/68030/68EC030/68040/68060 です。MMU、FPU（68881/68882/内蔵）にも対応しています。
- FDD、SCSI-HD、MO ドライブ、CD-ROM に対応しています。ディスクイメージは、FDD が XDF、SCSI-HD が HDS、CD-ROM が CloneCD（`.ccd` と `.img`）に対応しています。
- 男弾（XSP＋ZMUSIC＋PCM8A）、mint、NetBSD（MC68030/MC68060 モード）が動きます。
- [MCPサーバー](tools-host/mcp/MCP_TOOLS.md)を介し、Claude CodeやCodexなどのAIエージェントから画面・キーボード・ディスクを操作できます。IOCSの`_B_PUTC`と`_B_PRINT`をフックし、ソフトウェアが表示する文字をエージェント側が読めるテキストイベントとして取得できます。HIOCS.XやIOCS.Xが常駐していると出力が`TRAP #15`を通らなくなりますが、各ドライバの`_B_PUTC`本体へ採取点を自動で張るため（設定`text_auto_probe`、既定で有効）、そのまま取得できます。入力系の`_B_KEYINP`、`_B_KEYSNS`、`_B_SFTSNS`もフック／監視し、キー入力待ちを検出してエージェント側から文字・キー入力を送れます。
- `TEXT_PROBE`により、ゲームやNetBSDなどIOCSコールを使わずVRAM／BGTEXTへ直接書き込むプログラムでも、対応プロファイルを作成してソフトウェア固有の文字出力をエージェントが取得できます。
- それでも取得できない場合は、スクリーンショットを取得して、文字をパターンマッチングして取得します（半角英数のみ）。
- GUI版に加え、自動テストやMCP操作に使えるヘッドレス版をビルドできます。人間以外の X68000 ユーザーにもおすすめです。
- （ほぼ）C99とSDL3だけあればビルドできます。
- ホスト上の`m68k-xelf-gcc`と`elf2x68k`で`.X`形式の実行ファイルをクロスビルドできます（[クロスビルド](docs/x68k-usage/development/cross_build.md)）。X68000上のGCC.Xでのネイティブビルドにも対応しています（[ネイティブビルド](docs/x68k-usage/development/native_build.md)）。
- `HOSTDRV.SYS`により、ホスト側の作業ディレクトリをHuman68k側のドライブとして共有できます。MCPからは、稼働中のエミュレータとホストの間でファイルを1本ずつ転送するHOSTFILEも使えます（[ホストとゲストのファイル転送](docs/x68k-usage/guide/file_transfer.md)）。
- ディスクイメージを操作（フォーマット、ホスト側からのファイルの読み書き、fsck）する hu68disk を同梱しています。
  XDF／HDSイメージに対して、`list`・`copy`・`del`・`extract`・`mkdir`・`rmdir`・`mkfs`・`fsck`をコマンドラインから実行できます（[hu68disk](tools-host/hu68disk/README.md)）。
- エージェントから操作できるテキストエディタGhostED（GED.X）を同梱しています。
- **強力なデバッグ機能**: ブレークポイント、値変化ウォッチポイント、例外ブレーク、ステップ実行をサポートしています。
- プログラムカウンタの履歴をリングバッファに記録するPCRING、汎用レジスタの履歴を記録するREGRING、例外の履歴を記録するFAULTRINGがあります。
- ORI.B #$00,D0（命令コード$0000）を検出した時にログを出力するBADORI（XEiJ（STUDIO KAMADA）を参考にしました）、アドレスではなさそうな値（上位8ビットが$04〜$FE）がアドレスレジスタに代入された時にログを出力するBADMOVEAなどがあります。
- Human68k のエラー表示（白帯）を検出して実行を停止するWHITEBANDがあり、エラーが出た瞬間の状態をそのまま調べられます。
- 性能チューニング用に、指定した2つのPC間の仮想CPUサイクル数を測る`performance_counter`があります。ホスト負荷に依存しないので、最適化前後の比較に使えます。
- 上記機能を AI エージェントから使うことで、X68000 用プログラムの作成、デバッグを自律的に行えます（この辺が Intelligent）。

自動化、デバッグ機能の詳細は [FEATURES.md](FEATURES.md) を参照してください。

## 機能紹介動画
https://www.youtube.com/watch?v=v4xdfjl1tr0

## 動作条件

ビルド済みバイナリ（Releasesのアーカイブ）の動作条件です。

| OS | 条件 | 備考 |
| --- | --- | --- |
| macOS | **macOS 26 以降、Apple Silicon（arm64）のみ** | Intel Macと、macOS 15以前では起動しません。アーカイブの`ix68k.app`は未公証です（初回起動時の操作は、アーカイブ内の`README.md`を参照） |
| Linux | **x86_64、glibc 2.34 以上**（Ubuntu 22.04以降、Debian 12以降などが目安） | SDL3は同梱します。画面はX11またはWayland。音を出すには`libpulse0`が要ります（無いと音なしで動きます） |
| Windows | **Windows 10 以降（x64）** | SDL3とVCランタイム（`VCRUNTIME140.dll`）は同梱します。署名が無いので、SmartScreenの警告が出ることがあります |

動作を確認した環境は、macOS 26（Apple Silicon）、Windows 11、Ubuntu 22.04（WSL2、映像・音声は
dummyドライバのみ）です。上の条件を満たす別の環境や、Linuxの実際のX11・Waylandは、確認していません。
ソースからビルドする場合は、[ビルド・実行手順](docs/ix68k-internals/build.md)を参照してください。

IPLROM、CGROM、Human68kのディスクイメージは同梱していません。各自で用意します
（[QUICKSTART](QUICKSTART.md)）。

## クイックスタート

ビルド済みバイナリの入手、ROM・ディスクイメージの準備、設定、起動の手順は
[QUICKSTART.md](QUICKSTART.md) を参照してください。

### コーディングエージェントでインストールする

Claude CodeやCodexなど、ホストのシェルコマンドを実行できるコーディングエージェントに導入を任せる場合は、
次の固定手順を読ませて実行させてください。手順を都度考案させないため、毎回同じ結果になります。

1. エミュレータの新規導入: [emulator_install.md](docs/x68k-usage/mcp/provisioning/emulator_install.md)
2. 起動用SCSI HDDの作成: [hdd_install.md](docs/x68k-usage/mcp/provisioning/hdd_install.md)

依頼の例:

> docs/x68k-usage/mcp/provisioning/emulator_install.md を読んで、`~/ix68k` にエミュレータをインストールして

## X68000を使うための資料

ビルド、起動・終了、MCP操作、資料の使い方は [ix68k マニュアル](docs/x68k-usage/manual.md) にまとめています。

[X68000利用ガイド・ソフトウェア操作リファレンス](docs/x68k-usage/README.md)には、次の資料をまとめています。

- 初めてHuman68kを使うための通常利用ガイド
- MCP対応のAIエージェントによる操作、接続設定、環境構築レシピ
- Human68k付属コマンド・フリーウェアの操作リファレンス
- X68000上で動くアプリケーションの開発・デバッグ資料

エミュレータ本体の設計・実装資料は [アーキテクチャ](docs/ix68k-internals/architecture.md) を参照してください。

## ソースからビルドする

macOS、Linux、Windows、ヘッドレス版、テスト、設定ファイルを使った起動は[ビルド・実行手順](docs/ix68k-internals/build.md)を参照してください。

## サードパーティライセンス

- FM音源(OPM/YM2151)合成コア(`src/audio/ym2151.c`, `src/audio/ym2151_timer.c`,
  `src/audio/ym2151_tables.h`)は
  [ymfm](https://github.com/aaronsgiles/ymfm)(Copyright (c) 2021, Aaron Giles)を
  [XEiJ](https://stdkmd.net/xeij/)のJava移植経由でC言語へ再移植したものです。
  BSD 3-Clause Licenseの下で配布されています。ライセンス全文は
  [`LICENSES/ymfm-BSD-3-Clause.txt`](LICENSES/ymfm-BSD-3-Clause.txt)を参照してください。
- XEiJ由来のコード（上記のYM2151移植を含む）は、XEiJ Licenseの条件（無償配布、
  再コンパイルに必要なソースの同時配布、派生物への文書の添付）に従います。
  全文は[`LICENSES/XEiJ-License.txt`](LICENSES/XEiJ-License.txt)を参照してください。
- Human68k 3.02のFORMAT.X由来のブートコード（`tools-host/hu68disk/scsi_ipl_data.h`）と
  空きフロッピー`BLANK.XDF`は、無償公開されたシャープ製ソフトウェアの許諾条件に従います。
  全文は[`LICENSES/Sharp-Products-Users-Forum-License.txt`](LICENSES/Sharp-Products-Users-Forum-License.txt)を
  参照してください。
- [Berkeley SoftFloat 3e](https://github.com/ucb-bar/berkeley-softfloat-3)（John R. Hauser）:
  cmc68000経由でFPUの浮動小数点演算に使用しています。BSD 3-Clause License。
  全文は[`LICENSES/SoftFloat-BSD-3-Clause.txt`](LICENSES/SoftFloat-BSD-3-Clause.txt)を参照してください。
- [SDL3](https://github.com/libsdl-org/SDL): 画面・入力・音声に使用しています（zlib License）。
  リリースZIPには共有ライブラリとして同梱します。
- [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB):
  ゲームパッド定義（`gamecontrollerdb.txt`）をサブモジュールで取り込み、リリースZIPに同梱します（zlib License）。

## ライセンス

ライセンス検討中．フリーソフトウェアにする予定です．

## 謝辞

- X68000 の動作検証と YM2151 実装の参照元となった [XEiJ](https://stdkmd.net/xeij/) と STUDIO KAMADAは大いに参考にさせていただきました。
- X68000 向けクロス開発環境 [elf2x68k](https://github.com/yunkya2/elf2x68k) を公開している yunk 氏（yunkya2）。
- YM2151 音源コア [ymfm](https://github.com/aaronsgiles/ymfm) の Aaron Giles 氏。
- クロスプラットフォーム入出力を支える [SDL](https://libsdl.org/) と、ゲームパッド定義を提供する
  [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB) のコントリビューター。
- 資料とソフトウェアを保存・公開している X68000ユーザーの皆様。

## 参考文献

実装・検証では、次の資料およびプロジェクトを主に参照しています。

- 桑野雅彦著『Inside X68000』，ソフトバンク
- 桑野雅彦著『Outside X68000』，ソフトバンク
- 桑野雅彦著『X68030 Inside/Out』，ソフトバンク
- BEEPs著『X040turbo A Story of Making "After X68030"』，ソフトバンク
- シャープ『X68030 Human68k Ver 3.0 ユーザーズマニュアル』
- シャープ『X68030 日本語入力・辞書ユーティリティ ユーザーズマニュアル』
- シャープ『X68030 X-BASIC Ver 2.0 ユーザーズリファレンス』
- シャープ『X68000 C COMPILER PRO-68K ver2.0 プログラマーズマニュアル』
- シャープ『X68000 C COMPILER PRO-68K ver2.0 アセンブラマニュアル』
- 『X68k Programming Series #0: X680x0 Develop. & libc II』、
  『#1: X680x0 Develop. Vol. 1 Programmer's Guide / Vol. 2 Reference』、
  『#2: X680x0 libc Vol. 1 User's Reference / Vol. 2 Programmer's Reference』
- Motorola / Freescale Semiconductor、M68000 Family Programmer's Reference Manual、
  M68000、MC68010、MC68020、MC68030、MC68040、MC68060、MC68881、MC68882、
  MC68EC030 の各 User's Manual / Data Sheet
- Hitachi HD63450 DMA Controller Data Sheet、NEC µPD72065B FDC Data Sheet
- Ricoh RP5C01 / RP5C15 Application Manual、RP5C15 Data Sheet
- [Data Crystal: X68k/Assembler](https://datacrystal.tcrf.net/wiki/X68k/Assembler)、
  [DOSCALL](https://datacrystal.tcrf.net/wiki/X68k/DOSCALL)、
  [IOCS](https://datacrystal.tcrf.net/wiki/X68k/IOCS)、
  [IOMAP](https://datacrystal.tcrf.net/wiki/X68k/IOMAP)、
  [OSWORK](https://datacrystal.tcrf.net/wiki/X68k/OSWORK)、
  [PM](https://datacrystal.tcrf.net/wiki/X68k/PM)、
  [V2FUNC](https://datacrystal.tcrf.net/wiki/X68k/V2FUNC)
- [Inside X68000 非公式正誤表](https://kg68k.github.io/InsideX68000-errata/InsideX68000.html)
- [fj.sys.x68000 過去ログ（CRTC関連）](https://groups.google.com/g/fj.sys.x68000/c/OubVeaPH_oQ)
- [posfie: X68000の挙動に関する記録](https://posfie.com/@rentan_org/p/E6DLBPJ)
- [NetBSD/x68k 10.1 Live Image 20250106版](https://teokurebsd.org/netbsd/liveimage/20250106-x68k/)
- [KeyWitch.X 総合マニュアル](http://retropc.net/x68000/software/system/key/keywitch/keywitch.htm)
- [XEiJ](https://stdkmd.net/xeij/)
- [cmc68000](https://github.com/TerukiYamaguchi/cmc68000)
- [ymfm](https://github.com/aaronsgiles/ymfm)
- [SDL3](https://github.com/libsdl-org/SDL)
- [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)
