# アーキテクチャ概要

X68000 エミュレータ本体は C99 で実装し、MC68000 コアを git submodule
（`extern/cmc68000`）として利用します。画面・入力・音声は SDL3、テストや自動操作向けの
ヘッドレス構成は `platform_null` を使います。

## ファイル構成

```text
src/
├── main.c                    — 起動、メインループ、各サブシステムの統合
├── app/
│   ├── config.c/.h           — 設定ファイルとコマンドライン引数
│   ├── fpu_config.c/.h       — FPU構成（fpu=off|on|68881|68881-secondary|68882）の解釈
│   ├── keyboard_config.c/.h  — キー割当設定
│   ├── rom_check.c/.h        — IPLROMの起動前検証
│   ├── checkpoint.c/.h       — チェックポイント（状態保存・復元）の制御
│   ├── frame_timing.h        — 60HzフレームへのCPUサイクル有理数配分
│   └── runtime.c             — x68k_cycle_catchup()（周辺機器の時間同期）、FD0強制起動、NMI橋渡し
├── bus/
│   ├── x68k_bus.c/.h         — アドレスデコード、公開バスAPI
│   ├── bus_internal.h         — バスとペリフェラルの内部状態
│   ├── fpu_cir.c/.h          — FPU-CIR（CZ-6BP1）
│   ├── guest_clock.c/.h      — ゲスト時計（RTC・MFPの時刻源。デバッガ停止中は凍結）
│   ├── snapshot.c/.h         — 状態スナップショット
│   └── sram.c                — SRAMの初期化と永続化
├── peripheral/               — CRTC、DMAC、FDC、MFP、OPM、RTC、SASI/SCSI（scsi_media含む）、
│                                SCC、ADPCM（adpcm_codec含む）、Sprite、HOSTFILE/HOSTDRV
│                                （hostdrv_remote*）、SJIS、media_fp/sha256（媒体の同一性判定）
├── render/                   — GVRAM/TVRAM/スプライトの行描画とレイヤ合成、
│                                video_effects.c（R2半透明・特殊優先順位）
├── audio/                    — audio_mixer（ADPCM/OPMのリサンプル、ミキシング）、ym2151*（YM2151コア）
├── platform/
│   ├── platform.h            — プラットフォーム抽象API
│   ├── platform_sdl3*.c      — SDL3実装（画面、入力、音声、補助ウィンドウ）
│   ├── platform_macos_aux.m  — macOS固有の補助ウィンドウ制御
│   ├── platform_linux_aux.c、platform_win_aux.c — Linux/Windows固有の補助ウィンドウ制御
│   ├── debugger_launcher*.c  — 外部デバッガ（tools-host/debugger）の起動
│   ├── aux_font.c/.h         — 補助ウィンドウ用フォント
│   ├── win_compat.c/.h       — Windowsネイティブビルド用POSIX互換層
│   └── platform_null.c       — ヘッドレス実装
├── ui/                       — ディスク・ソフトウェアキーボードウィンドウ
├── mcp/
│   ├── server.c              — 接続（macOS/LinuxはAF_UNIX、WindowsはTCPループバック）、フレーム分割、応答送信
│   ├── sock_path.c/.h        — AF_UNIXソケットパスの環境変数解決
│   ├── port.c/.h             — Windows TCPポート番号の環境変数解決
│   ├── commands.c/.h         — 即時・待機コマンドと非同期poll
│   ├── debugger.c/.h         — CPUデバッガ統合
│   ├── dbg_cond.c/.h         — 条件付きブレークポイントの条件式パーサ・評価器
│   ├── screenshot.c/.h       — PNG応答生成
│   ├── perf.c/.h             — performance_counter（サイクル計測。累積カウンタと区間計測）
│   ├── text_event.c/.h,
│   │   text_probe.c/.h       — テキストイベント検出とプロファイル照合
│   └── internal.h            — MCP内部共有定義
├── app_internal.h            — main.c/mcp/server.c/iocs.cの3ファイル間共有状態
└── iocs.c                    — Cで処理するIOCSとROM委譲のディスパッチ
```

## 主な依存関係

```text
main/app → bus, render, audio, platform, mcp, mc68000
bus      → peripheral, mc68000
render   → busの公開描画データ（CPU・SDLには非依存）
audio    → OPM/ADPCMの生成サンプル（SDLには非依存）
platform → SDL3、またはPLATFORM_NULL時は外部GUI依存なし
mcp      → bus, mc68000デバッグAPI, platform
```

ペリフェラルの内部状態は `src/bus/bus_internal.h` に集約し、外部からは原則として
`src/bus/x68k_bus.h` の公開APIを通して操作します。MCPサーバはソケット処理とコマンド処理を
分離し、待機中の状態は `mcp_context_t` が保持します。RTC の時計もバスに所有させ、MMIO と
IOCS の日付・時刻APIは同一の時刻源を共有します。

## 実行モデル

1フレームのCPUサイクル数は `frame_timing.h` の有理数累算で決めます。実行中は
`MFP_TICK_CYCLES` 単位で CPU を進め、`x68k_cycle_catchup()` が MFP、CRTCラスター、OPM、
ADPCMを同じ経過サイクルで更新します。CPUが STOP/HALT 中でも、タイマとラスターは実時間として
進行します。

CRTCが確定したラスタ行は、その時点のレジスタ状態で `compose.c` が合成します。OPMはCPU進行に
比例して62.5kHzのサンプルを生成し、ADPCMと出力レートへリサンプルして加算します。最終的な
音声キューと画面表示は platform 層だけが扱うため、ヘッドレス構成ではエミュレーション状態を
変えずに出力だけを破棄できます。

## mc68000 サブモジュール

`extern/cmc68000/` は独立した静的ライブラリ。通常実行APIに加え、MCPデバッガから
`include/mc68000/debug.h` のブレークポイント、ウォッチポイント、非侵襲peek、履歴リングを
利用します。詳細は `extern/cmc68000/AGENTS.md` を参照。

## 描画モデル

ROMがTVRAM/GVRAM/スプライトRAMへ書いた内容を、CRTCのラスタ進行に合わせて行単位で合成します。
`src/render/compose.c` がビデオコントローラの表示ON/OFF・優先順位・コントラストを解釈し、
各レンダラを呼び分けます。R10のテキスト水平スクロールはライブ値、R11およびR13/R15/R17/R19
の垂直スクロールはV-DISP開始時のラッチ値を使う（`-raster-y live`指定時を除く）。

TVRAM書き込みではCRTC R21の同時アクセスとR23のアクセスマスクを適用します。グラフィックの
色形式はGRB555で、`0x0000`を透過色、黒を`0x0001`として扱います。

CRTCの走査・スクロール、ビデオコントローラのR1/R2・パレット・コントラスト、スプライト
コントローラの責務は別チップとして分離します。詳細は
[crtc_design.md](design/crtc_design.md)、
[video_controller_design.md](design/video_controller_design.md)、
[sprite_design.md](design/sprite_design.md) と対応する `support/` 文書を参照します。

## 関連設計書

設計上の決定は `design/`、実装状態とレジスタ対応は `support/` を正本とします。
主な周辺機器は [RTC](design/rtc_design.md)、[MFP](design/mfp_design.md)、
[OPM](design/opm_design.md)、[ADPCM](design/adpcm_design.md)、
[FDC](design/fdc_design.md)、[SCSI](design/scsi_design.md)、
[HOSTDRV](design/hostdrv_design.md)、[MCP](design/mcp_design.md)、
[バスアクセス時間](design/bus_timing_design.md) を参照します。
