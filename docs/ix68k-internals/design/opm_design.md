# FM音源（OPM / YM2151）設計

## 1. 目的と構成

ix68k は X68000 の OPM（YM2151）を、音源コアと X68000 固有の配線に分離して実装します。

| 層 | 主なファイル | 責務 |
|---|---|---|
| YM2151 コア | `src/audio/ym2151.c`、`ym2151_timer.c`、`ym2151_tables.h` | 8 チャンネル／4 オペレータ、PG/EG/LFO/ノイズ、タイマ、ステータス、62500 Hz ステレオ生成。バス・CPU・MFP に依存しない |
| OPM グルー | `src/peripheral/opm.c` | レジスタポート、BUSY、CPU サイクルから OPM クロックへの変換、FMIRQ 生線 |
| X68000 配線 | `src/bus/x68k_bus.c`、`src/peripheral/mfp.c` | MMIO、MFP GPIP3 への FMIRQ 接続、IRQ6 割り込み |
| 音声出力 | `src/main.c`、`src/audio/audio_mixer.c` | OPM／ADPCM のリサンプリング、加算、最終クリップ、SDL キュー出力 |

YM2151 コアは ymfm を XEiJ の Java 移植経由で C に再移植したもので、BSD-3-Clause の
ライセンス表記は `LICENSES/ymfm-BSD-3-Clause.txt` に保持します。

## 2. レジスタとバス

- OPM 領域は `$E90000-$E91FFF`。`$E90001` がアドレスラッチ、`$E90003` がデータポートです。
- 読み出しは A0 を区別せず、bit7=BUSY、bit1=Timer B、bit0=Timer A のステータスを返します。
- データ書き込みは YM2151 コアへ渡し、`$14`（モード）以外では 64 OPM クロックの BUSY を設定します。
- `$1B`（CT1/CT2）は OPM コアにも渡す一方、ADPCM のクロック切替と FDC READY 用に
  `adpcm.c` が同じ生バイトを独立して追跡します。この二重管理は意図的です。
- `_OPMSET`、`_OPMSNS`、`_OPMDRV`、`_OPMINTST` は ROM へ委譲します。

## 3. 音源コア

- YM2151 の 8 チャンネル、各 4 オペレータ、接続アルゴリズム CON=0-7、L/R パン、
  PG/EG/LFO／ノイズを実装します。
- 生成形式は 62500 Hz、ステレオ `int32_t`。合成後に `±32767` へ収めます。
  YM3012 DAC 固有の量子化再現は未実装です。
- レジスタ `$14` の LOAD A/B はエッジトリガでタイマを開始／停止します。IRQEN A/B は
  以後のオーバーフローでステータスフラグを立てるゲートであり、既に立ったフラグを
  マスクしません。FRESET A/B は対応フラグをクリアします。
- OPM 内蔵 Timer A/B と MC68901 内蔵 Timer A/B/D は別機能であり、混同しません。

## 4. 時間進行と FMIRQ

OPM は 4 MHz で動作します。`opm_tick()` は CPU サイクルを OPM クロックへ既約分数で変換し、
端数を累積してタイマを進めます。CPU クロックの変更は `opm_set_cpu_hz()` で反映します。

タイマ進行は `x68k_bus_mfp_tick()` と同じ CPU 実行チャンク粒度で行い、フレーム単位の
音声生成とは分離します。これにより音声バッファの長さによってテンポや割り込み周期が変わることを防ぎます。

FMIRQ は MFP の GPIP3（IPRB/IERB/IMRB bit3）へ配線し、IRQ6 で通知します。MFP 内の優先順位は
USART RX（ch12）> Timer C（ch5）> FMIRQ（ch3）。FMIRQ の pending は専用ラッチで管理し、
割り込み acknowledge 時に `VR | 3` を返します。

## 5. 音声ミキシング

OPM と ADPCM は各ソースを出力レートへリサンプリングし、`int32_t` ステレオで加算します。
クリップは `audio_mix_sum_clip_stereo()` の最終段だけで行います。OPM は 62500 Hz、ADPCM は
CT1 により変化するネイティブレートを用います。出力側のフレーム数と OPM 生成サンプル数は
60 Hz の端数累積で維持します。

ADPCM 自体のレジスタ、DMA、録音経路は `adpcm_design.md` を参照します。

リサンプラは `ix68k.cfg` の `resample_mode` で選ぶ（既定は linear）。フレーム境界の
補間連続性のため、OPM は先読みサンプルを次フレームへ持ち越します。

## 6. 実装状況と制約

| 項目 | 状態 |
|---|---|
| OPM レジスタ、BUSY、ステータス | ✅ |
| YM2151 合成、L/R パン、LFO、ノイズ、CON=0-7 | ✅ |
| Timer A/B、MFP GPIP3(FMIRQ)、IRQ6 | ✅ |
| OPM と ADPCM の加算 | ✅ |
| CSM（`$14` bit7）キーオントリガ | ❌ 未実装 |
| YM3012 DAC 固有の量子化再現 | ❌ 未実装（恒等処理） |
| 実機の垂直帰線期間だけレジスタ更新を有効にする精密タイミング | ❌ 未実装 |

詳細な実装対応表は `docs/ix68k-internals/support/opm_support.md` を正本とします。

## 7. 検証

- `ym2151_tables_test`、`ym2151_gen_test`：テーブルとコア生成。
- `opm_timer_test`：Timer A/B、BUSY、IRQEN／FRESET。
- `mfp_fmirq_test`：GPIP3 と IRQ6 の配線、MFP 内優先順位。
- `audio_mixer_test`：リサンプリング、加算、最終クリップ。
- 実ソフトでは FM 音、テンポ、L/R パンを XEiJ と比較します。

## 変更履歴

| 日付 | 内容 |
|---|---|
| 2026-09-11 | 旧実装計画から現行仕様を統合 |
