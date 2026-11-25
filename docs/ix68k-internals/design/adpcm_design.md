# ADPCM（MSM6258V）設計

## 1. 構成と責務

ix68k は MSM6258V の再生・録音と、DMAC ch3 による実時間転送を実装します。

| 層 | 主なファイル | 責務 |
|---|---|---|
| MSM6258 | `src/peripheral/adpcm.c`、`adpcm_codec.c` | コマンド／ステータス、4-bit ADPCM の encode/decode、PPI・OPM 制御、入出力リング |
| DMA | `src/peripheral/dmac.c` | ch3 のサンプルレート同期転送、チェーン、停止／一時停止／再開、完了割り込み |
| 音声変換 | `src/audio/audio_mixer.c` | リサンプリング、パン、OPM との加算 |
| ホスト I/O | `src/main.c`、`src/platform/platform_sdl3_audio.c` | SDL3 出力・マイク入力、バッファ管理 |

## 2. レジスタと周辺制御

| 対象 | アドレス | 役割 |
|---|---|---|
| MSM6258 コマンド／ステータス | `$E92001` | REC ST、PLAY ST、SP 停止。優先順位は STOP > PLAY > REC |
| MSM6258 データ | `$E92003` | 再生時は 1 バイトから 2 サンプルを decode、録音時は入力 PCM を encode |
| PPI ポート C | `$E9A005` | bit3-2=サンプルレート分周、bit1-0=パン（ともに ADPCM 関連） |
| OPM `$1B` | `$E90001/$E90003` | CT1=ADPCM 基本クロック、CT2=FDC READY 強制 |

PPI のパンはアクティブローで、bit0 が左、bit1 が右です。CT1 と CT2 は OPM の
書き込みを `adpcm_opm_write8()` が独立に追跡します。CT1 は実効サンプルレート、CT2 は
FDC の物理ドライブ・プローブ時の READY 強制に用います。

## 3. DMAC ch3 と時間進行

ADPCM の DMA は ch0--2 と異なり、CCR START 時に一括転送しません。DMA 設定を
`adpcm_dma_t` にラッチし、`x68k_bus_adpcm_tick()` が CPU 実行チャンクごとに、
サンプルレートと経過 CPU サイクルに応じたバイト数を漸進転送します。

- 1 バイトは ADPCM 2 サンプルです。
- 転送予算は端数累積し、CPU クロック変更時にも実時間のドリフトを残しません。
- アレイ／リンクアレイチェーンはエントリ境界をまたいで状態を保持します。
- CCR.CNT による連続動作、`_ADPCMMOD` の SAB 停止・HLT 一時停止・再開を扱います。
- 完了時は CSR.COC と DMAC ch3 割り込みを立て、ベクタ `$6A` で ROM 側の待機を解除します。

DMAC のレジスタ詳細と ch3 の状態機械は `docs/ix68k-internals/support/dmac_support.md` を正本とします。

## 4. 再生経路

`$E92003` への DMA 書き込みで得た PCM サンプルを、1024 サンプルの `audio_ring` に積みます。
メインループはこれを取り出し、ADPCM ネイティブレートからホスト出力レートへリサンプリングし、
PPI パンを適用します。OPM と同時再生する場合は、両方を `int32_t` ステレオで加算してから
最終段だけを 16-bit へクリップします。

リサンプラは nearest／linear／Catmull-Rom cubic を選択でき、既定は linear です。
フレーム境界をまたぐ位相と前回サンプルを保持して、補間の不連続を避けます。

ホスト出力は SDL3 のプッシュ型 `SDL_AudioStream` を使います。キューが過剰に滞留した場合は
破棄して事前バッファリングから再開し、枯渇後も未来のゲスト CPU／音声を先行生成せず無音で回復します。
ヘッドレス環境では出力のみ破棄し、コーデック、DMA、割り込みは通常どおり進めます。

## 5. 録音経路

マイク入力は 44100 Hz モノラルで取得し、ADPCM の現在のネイティブレートへ変換して
1024 サンプルの `rec_ring` に積みます。録音モードでの `$E92003` 読み出しはこのリングから
PCM を取り出して encode します。

マイク未接続、権限拒否、入力リング枯渇時はゼロを encode する無音フォールバックとします。
CPU デバッグ停止中もホストの capture キューを滞留させないため、マイク入力のドレインは継続します。

## 6. 検証

- `adpcm_codec_test`：codec の encode/decode。
- `adpcm_test`：MSM6258 のレジスタ、モード、PPI／OPM 制御。
- `adpcm_dma_test`：ch3 の実時間転送、チェーン、停止・再開、完了割り込み。
- `audio_mixer_test`：再生・録音リサンプリング、パン、最終クリップ。
- 動作検証では `_ADPCMOUT` の再生と `_ADPCMINP` の録音・再生ループバックを確認します。

## 変更履歴

| 日付 | 内容 |
|---|---|
| 2026-09-11 | ADPCM の現行仕様を独立設計書として整理 |
