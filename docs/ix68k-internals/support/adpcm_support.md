# ADPCM (MSM6258V) 音声入出力 実装状況

現行設計は `docs/ix68k-internals/design/adpcm_design.md`。`src/peripheral/adpcm.c` + `src/audio/audio_mixer.c` + `src/platform/platform.h`の
`pal_audio_*` API。関連: DMAC ch3は`docs/ix68k-internals/support/dmac_support.md`、OPMとのミキシングは
`docs/ix68k-internals/support/opm_support.md`を参照。

> **必須**: ADPCM の音声パス・DMA連携を実装・変更したら、このファイルを必ず更新します。

---

## レジスタ単位の存在監査（2026-07-29）

MSM6258V自体が持つレジスタは『Inside X68000』図32によれば実質2本のみ
（他はPPI ポートC・OPM CT1で代替）:

| レジスタ | アドレス | 実装状況 |
|---|---|---|
| ステータス/コマンド | $E92001 | ✅ 全ビット実装済み（bit2=REC ST/bit1=PLAY ST/bit0=SP停止、`adpcm.c`で優先順位STOP>PLAY>RECも明記の上で処理） |
| データ入出力 | $E92003 | ✅ 実装済み（2サンプル/バイト、再生・録音とも） |

小さいレジスタセットで既に全ビット実装済みのため、今回の監査で新規ギャップは
見つからありませんでした。

## ADPCM音声出力（2026-07-15）

`src/audio/audio_mixer.c`（バス/CPU/PAL非依存の純ロジック、`src/render/`配下
と同じ設計方針）+ `src/platform/platform.h`の`pal_audio_*` API（SDL3実装は
`platform_sdl3_audio.c`、ヘッドレスは`platform_null.c`で常時無効）。

- **出力経路**: `adpcm.c`の`$E92003` write(再生方向)がデコードした各サンプルを
  `adpcm_t.audio_ring`（1024サンプルの循環バッファ）へpushします。`main.c`が
  毎フレーム`x68k_bus_adpcm_tick()`の直後に`x68k_bus_adpcm_take_samples()`で
  吸い出し、`audio_mix_resample_pan()`で固定レート(44100Hz)・ステレオへ変換して
  `pal_audio_queue()`（SDL3版は`SDL_PutAudioStreamData`）へ渡します。
- **リサンプル**: 最近傍・線形・Catmull-Rom 3次補間を`resample_mode`で選択でき、
  既定は線形補間。Q16.16の位相と直前サンプルを呼び出し間で保持し、フレーム境界の
  位相ずれや補間切れを避けます。録音側はアップ/ダウンサンプルの両方向に対応します。
- **パンポット**: PPIポートC(`$E9A005`)bit1-0（アクティブロー、
  『Inside X68000』3-2-5節/図36: bit0=左bit1=右、'1'=OFF・'0'=ON）を
  `adpcm_get_pan()`で論理値へ変換し、L/Rそれぞれのゲイン(0または1)として適用。
  両方OFFなら無音、両方ONなら中央（L=R）。
- **SDL3の`SDL_AudioStream`をプッシュ型で使用**します。`SDL_OpenAudioDeviceStream`を
  コールバック無しで開き、`SDL_PutAudioStreamData`でメインループから供給します。
- **出力キューの滞留上限(約0.5秒, `AUDIO_MAX_QUEUE_BYTES`)**: プッシュ型では
  生成量と消費量が1:1で釣り合う前提だが、ホスト側でメインループが長時間止まる、
  またはオーディオデバイスがキューを消費しない期間（ウィンドウ操作・スリープ・
  出力デバイス切り替え等）があると滞留が残る。SDLは滞留を捨てないため、
  滞留量がそのまま恒久的な音の遅延になる（実測で147秒分=約26MBまで成長し、
  ZMUSICの再生開始・停止が同じだけ遅れた）。上限超過時はキューを破棄して
  事前バッファリング(`AUDIO_PREBUFFER_BYTES`, 約0.2秒)からやり直します。
- **枯渇後の回復**: 再生中に次のPCMを積む直前のSDLキュー残量が0なら、
  `pal_audio_queue()` は一度pauseして事前バッファリングへ戻る。論理CPUや
  OPM/ADPCMを音声補充目的で先行させず、再開までの短時間は無音として扱います。
  `pal_audio_queued_frames()` は残量をフレーム数で返す診断用PAL APIであり、
  取得不能時は負値を返します。通常終了時は`audio stats:`としてreset／underflow／
  queue-limitの各回数を出力するため、負荷試験の受入ログではunderflow=0を
  確認します。
  併せて`main.c`のフレームペーシングも、遅れが`FRAME_CATCHUP_LIMIT_US`
  (100ms)を超えたら取り戻しを諦めて現在時刻へ同期し直す
  （全速の取り戻し中に60Hzを超える速度で音声を生成しないため）。
- **リセット時のキュー破棄**: `app_reset_machine()`（メニューのCmd+R・
  Cmd+Shift+R、MCPの`RESET`/`RESET_FD0`共通）は`pal_audio_reset()`で出力キューを
  捨てます。周辺(OPM/ADPCM)を止めてもSDLへ積んだ分は残るため、捨てないと
  リセット前の音が滞留分だけ鳴り続けます。
- **描画負荷への対処**: 論理フレームごとにCPU・MFP・CRTC・
  OPM・ADPCM・PCM生成を必ず一緒に進め、deadlineを過ぎた次フレームだけ描画を
  省略します。ラスタ走査・V-DISP・タイマはスキップしないため、描画の有無で
  ゲスト／音源の時系列は変わりません。既定は自動スキップで、`-frameskip off`は
  比較用、`-render-load-us N`は描画完了後にNµsの再現用負荷を加える診断専用
  オプションです。`-run-for-sec N`はN秒後に通常の終了経路へ進める受入試験用
  オプションで、最後に`audio stats:`を得られます。`-frame-load-us N`は描画を
  省略しても残るホスト負荷を加え、hard overloadの診断を再現します。キュー残量を
  理由に未来のCPU／PCMを生成してはなりません。
- **hard overload**: 描画を省略した論理フレームでもdeadlineを逃した場合は、
  CPU・周辺・音声自体が60Hzに追いつかない適用限界です。音声を未来へ水増しせず、
  `hard-overload:`を毎秒最大1行だけ出力して診断します。
- **CPU周期の端数**: CPUクロックは60Hzフレームへ有理数累算で配分します。
  CPU実行量とCRTCのフレーム走査量は常に同一の配分値を使い、10〜50MHz設定で
  長時間のゲスト時刻ドリフトを蓄積しません。
- **`PLATFORM_NULL`（ヘッドレス）**: `pal_audio_available()`は常に0。ch3 DMA・
  コーデック・割り込みは通常どおり動作させ、出力のみ破棄する（既存の
  PAUSE→NMIテストと同じ切り分け方針）。
- 単体テスト`audio_mixer_test`: リサンプル比率(65サンプル@3900Hz→約735フレーム
  @44100Hz)、パンon/off時のL/R値、`out_capacity_frames`頭打ちを検証。
- 動作検証: 使い捨てアセンブリテスト（`.incbin`で`disk/otoko100_sound/BEAM.PCM`
  を直接埋め込み）で`_ADPCMOUT`(ウェイトモード、左右ともON)を実行し、
  SDL音声デバイスが正常初期化（失敗ログなし）の上で実際にスピーカーから
  効果音が再生されることをユーザーが確認。ビルド警告ゼロ・`ctest`全通過
  （SDL3版・ヘッドレス版とも）。

---

## ADPCM録音入力（2026-07-15）

`_ADPCMINP`等（マイク入力経路、ユーザー決定事項）。出力と対称な
設計で、`pal_audio_capture_*`（SDL3は録音用`SDL_AudioStream`+
`SDL_GetAudioStreamData`、ヘッドレスは常時無効）+ `audio_mixer.c`の
`audio_resample_mono()`（出力側と同じBresenham型誤差蓄積アルゴリズムで
アップ/ダウンサンプル双方向対応）+ `adpcm.c`の入力リング(`rec_ring`)で構成。

- **入力経路**: `main.c`が毎フレーム`pal_audio_capture_dequeue()`で
  マイクcapture(固定44100Hzモノラル)を取得し、`audio_resample_mono()`で
  ADPCMネイティブレート(`x68k_bus_adpcm_sample_rate_hz()`)へダウンサンプル
  してから`x68k_bus_adpcm_push_rec_samples()`で`adpcm_t.rec_ring`
  （1024サンプルの循環バッファ）へpushします。`$E92003` read(録音方向)は
  この入力リングからpopしたサンプルを`adpcm_codec_encode()`に渡します。
- **マイク入力ドレインはCPUデバッグ停止中も毎フレーム実行**（出力と異なり
  halt中に止めるとSDL側のcaptureキューが実時間で無制限に溜まってしまう
  ため）。halt中にpushしても`$E92003`を読むCPU自体が止まっているため無害。
- **無音フォールバック**: マイク未接続・macOS TCC権限拒否・単に無音区間
  いずれの場合も入力リングが空になるだけで、pop側が0を返し初期実装以来の
  「無音をencodeする」動作へ自然にフォールバックする（追加の特別処理は
  不要な設計）。
- 単体テスト`audio_mixer_test`に`audio_resample_mono()`のダウンサンプル
  （44100Hz→15625Hz相当）・アップサンプル対称性を追加。
- 動作検証: 使い捨てアセンブリテスト（`_ADPCMINP`で録音→同じバッファを
  `_ADPCMOUT`で再生するループバック）で、一時計装（`fprintf`でマイク
  capture・encode()直前の実サンプル値を計測）により発話時に実際の音声
  レベル（ピーク700-1800/32767）がencode()まで正しく到達することを確認。
  録音音量はマイク直接入力のため出力用サウンドデータより小さいが、
  ユーザーが実際にスピーカーから録音内容の再生を確認。一時計装は削除済み。
  ビルド警告ゼロ・`ctest`全通過（SDL3版・ヘッドレス版とも）。

## 変更履歴

| 日付 | 内容 |
|------|------|
| 2026-08-23 | deadline超過時の描画スキップ、SDL出力キューの枯渇後の再バッファ、CPU周期の60Hz端数累算を追加。`test_audio_recovery`と`frame_timing_test`を追加し、Debug/RelWithDebInfoで全テスト成功。 |
| 2026-07-15 | ADPCM音声出力を実装。`src/audio/audio_mixer.c`（最近傍リサンプル+パン合成の純ロジック）・`pal_audio_*`を新設。現在のSDL3実装は`SDL_AudioStream`を使用し、ヘッドレスでは出力を破棄します。ix68kで`disk/otoko100_sound/BEAM.PCM`の再生音を確認 |
| 2026-07-15 | ADPCM録音入力を実装。`pal_audio_capture_*`・`audio_resample_mono()`・`adpcm.c`の`rec_ring`を新設。ユーザーが録音→再生ループバックの音声を実際に確認 |
| 2026-07-16 | アレイ/リンクアレイチェーン(_ADPCMAOT/AIN/LOT/LIN)と`_ADPCMMOD`(一時停止/再開)を実装。詳細は`docs/ix68k-internals/support/dmac_support.md`のch3節参照 |
| 2026-07-25 | `x68k_bus_adpcm_tick()`をフレーム単位(60Hz)からCPUチャンク単位(`MFP_TICK_CYCLES`粒度)呼び出しへ変更（「taken=0の実ギャップ」修正）。音楽ドライバのCCR SAB→数千サイクル後CCR START再武装がフレーム境界を跨ぐと、旧実装ではそのフレームが丸ごと0バイト転送になり実機の0.4msギャップが16.7msの音切れへ拡大してきました。転送予算をフレーム単位Bresenham(`fs/120`)からサイクル単位Bresenham(`fs*cycles/(2*CPU_HZ)`)へ置き換え。回帰確認用に`tests/adpcm_dma_test.c`を新設 |
| 2026-07-29 | `docs/hw_support.md`から本ファイルへ分離（ペリフェラル単位でのファイル分割） |
