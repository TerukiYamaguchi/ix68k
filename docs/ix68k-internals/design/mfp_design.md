# MFP（MC68901）設計

## 1. 目的と対象範囲

ix68k の MFP（`$E88001–$E8802F`）は、GPIP 入力、タイマ、USART 受信、
割り込み要求・受付を担います。本書はその現行仕様を定めます。

- 対象: MFP レジスタ、実装済み割り込みチャンネル、GPIP 信号、タイマ、USART RX、
  IPR/ISR/IMR/VR と IACK。
- 対象: 上記に加え、UDR 書き込み（USART TX）で受けるホスト→キーボードのコマンド。
- 対象外: FDC／OPM が信号源となる周辺機器の詳細、USART の送信タイミング・エラー処理。
- レジスタとチャンネル別の対応状況は [`mfp_support.md`](../support/mfp_support.md) を正本とします。

## 2. 実装状態

| 項目 | 状態 | 補足 |
| --- | --- | --- |
| 割り込み配送・固定優先度 | ✅ | `ch_pending` と IACK で ch15→ch0 を選ぶ |
| GPIP3/4/6/7 | ✅ | FMIRQ、V-DISP、CIRQ、H-SYNC を生線として反映 |
| Timer B/C/D | ✅ | ディレイモード。Timer B のイベント計数は未対応 |
| Timer A | ⚠️ | V-DISP 終端のイベントカウント近似のみ |
| USART RX | ✅ | キーボード用 FIFO と UDR ラッチ |
| ソフトウェア EOI | ✅ | 起動オプションで S=1 を有効化可能 |
| USART TX（UDR 書き込み） | ⚠️ | キーボードコマンドをデコードして保持。送信時間・TSR BE 遷移は未再現 |
| DDR、SCR、UCR、USART TX エラー | ❌ | 一部は意図的に省略 |

## 3. 設計の要点

1. MFP の保留状態は IPR、CPU への配送候補は `ch_pending` に分けます。前者はイベントを
   ラッチし、後者は IER/IMR と In-Service の条件を満たしたチャンネルだけを表します。
2. GPIP は保留ビットではなく入力ピンの生レベルです。CIRQ、FMIRQ、V-DISP、H-SYNC
   は、それぞれの信号源から直接合成します。
3. IACK は最高優先度の配送候補を一つだけ受理し、IPR を落とします。VR の S=1 では ISR に
   記録し、より低優先度の配送をブロックします。
4. タイマのディレイ計数は CPU クロックに依存しないよう、4 MHz MFP クロックとの有理数
   累積で進めます。

## 4. 構成とデータモデル

`x68k_bus_t::mfp`（`mfp_t`）がレジスタ生値、タイマの現在値と端数、USART FIFO、
`ch_pending` を所有します。`src/peripheral/mfp.c` が MMIO とイベント更新を、
`src/bus/x68k_bus.c` が IRQ レベル判定と IACK を担当します。

| 状態 | 用途 |
| --- | --- |
| IERA/B・IPRA/B・ISRA/B・IMRA/B・VR | 要因の有効化、ラッチ、In-Service、マスク、ベクタ |
| `ch_pending` | CPU が受け付け可能な MFP チャンネル集合 |
| Timer A/B/C/D 状態 | 制御値、初期値、現在カウンタ、CPU サイクル端数 |
| USART RX FIFO／UDR ラッチ | キーボード入力と受信バッファフル状態 |
| `usart_tx_count` | UDR 書き込みの累計バイト数（観測用。`mfp_reset` で消さない） |
| `x68k_bus_t::kbd`（`kbd_state_t`） | キーボード本体の LED・LED 明るさ・リピート設定。MFP ではなく外部デバイスの状態なので `mfp_t` に置かず、`mfp_reset` でも消さない |
| `gpip_raster_enabled` | GPIP4/7 の CRTC ラスター由来化の有効／無効 |

## 5. レジスタと割り込み

### 5.1 GPIP とエッジ

GPIP はアクティブ Low で読みます。bit0 は現在、内蔵 FDD 存在として固定 Low です。

| bit | 信号 | ix68k の入力源 |
| --- | --- | --- |
| 7 | H-SYNC | CRTC のライン内同期期間 |
| 6 | CIRQ | 現在ラスタと CRTC R09 の一致 |
| 4 | V-DISP | CRTC の垂直表示期間 |
| 3 | FMIRQ | OPM の `fmirq_line` |

CRTC ラスターモデルが利用不能な場合、または `-mfp-gpip-raster off` 指定時は、
GPIP4/7 をポーリング用の位相近似へフォールバックします。これは ROM が両レベルを待つため、
固定値にできないためです。AER のエッジ方向は現状、割り込み発生に反映しません。

GPIP7 は各ラインで、フロントポーチ後の H-SYNC パルス期間だけ High となります。GPIP4 は
V-DISP 開始から終了まで High です。CRTC ラスターが有効な通常時、GPIP 読み出し自体に
副作用はありません。フォールバック時だけ `gpip_fallback_phase` を進め、両極性の待ちループが
停止しないことを優先します。

### 5.2 割り込み制御

IER は要因のラッチ可否、IMR は CPU への配送可否を決めます。IPR と ISR への書き込みは
AND 意味論であり、0 を書いたビットだけをクリアします。IMR/IPR を書き換えた後は、
実装済みチャンネルを再評価して `ch_pending` と同期します。

VR の上位 4 bit とチャンネル番号がベクタを構成します。S=0 では自動 EOI 相当、S=1 では
IACK が ISR をセットし、最上位 In-Service より低いチャンネルを配送しません。既定では
`-mfp-soft-eoi off` により S=0 固定です。

| S | IACK 後の ISR | 次の配送 |
| --- | --- | --- |
| 0 | セットしない（自動 EOI） | 全チャンネルを通常どおり選択 |
| 1 | 受理したチャンネルをセット | 同一・下位優先度を EOI までブロック |

ISR と IPR は、ソフトウェアが 1 を書いてセットできません。EOI は該当 ISR ビットへ 0 を
書くことで行います。bit2–0 は未使用で、VR 読み出しでは 0 となります。

### 5.3 タイマと USART

Timer B/C/D は分周比 4/10/16/50/64/100/200 のディレイモードを実装します。値 0 は
8 bit カウンタとして 256 を意味し、アンダーフロー時には初期値を再ロードします。
Timer A は V-DISP 終端でのイベント計数だけを実装します。

Timer B は ch8（IERA/IPRA/ISRA/IMRA bit0）です。標準設定は TBCR=`$01`、
TBDR=`$0D`（4 MHz ÷ 4 ÷ 13）だが、IPLROM は bit0 をマスクするため通常は IRQ6 を
発生しません。TBI が Low 固定のためイベントカウントは停止扱いとし、パルス幅測定と
TBO の USART 伝送クロック配線も実装しません。

USART RX はホスト入力を FIFO へ積み、IERA/IMRA bit4 が揃うと UDR ラッチへ昇格します。
2400 bps・10 bit/バイト相当の最短間隔を守り、UDR 読み出しはラッチ値を返して BF を落とします。
受信エラー、シリアルフレーミングは実装しません。

#### USART TX（キーボードコマンド）

ROM の `_KEY_INIT`／`_LEDCTRL`／`_KEYDLY`／`_KEYREP`／IOCS `$0A`・`$0B` は、共通ルーチン
`$ff6502` で TSR bit7 を待って UDR へ 1 バイト書きます。ix68k は TSR を常時 ready とし、UDR
書き込みを受けた瞬間にデコードして `x68k_bus_t::kbd` へ保持します。保持値は生のコマンド値で、
ms 換算はしません。現状、保持値の用途は観測のみで、ホスト側のキーリピートや UI には反映しません。

| コマンド | 意味 | 扱い |
| --- | --- | --- |
| `$80-$FF` | LED。`~値` の下位 7 bit、1 で点灯。bit0 かな／1 ローマ字／2 コード入力／3 CAPS／4 INS／5 ひらがな／6 全角 | `led` に保持 |
| `$54-$57` | LED 明るさ（下位 2 bit） | `led_bright` に保持 |
| `$60-$6F` | リピート開始時間（200+100n ms、下位 4 bit） | `repeat_delay` に保持 |
| `$70-$7F` | リピート間隔（30+5n² ms、下位 4 bit） | `repeat_rate` に保持 |
| `$40-$53`、`$58-$5F` | MSCTRL（`$40`/`$41`）、ロック、TV 制御（`$5C`/`$5D`）等 | 状態を持たず、カウンタのみ |

マウス処理（`$ff16ce` 付近）は `$40`/`$41` を周期的に送るため、UDR 書き込みごとのログ出力は入れません。
RX 側（`usart_rx_*`、IPRA、`ch_pending`）には触れません。

## 6. 動作仕様

MFP 要因が発生すると、IER が有効な場合に対応する IPR をセットします。IMR も有効であり、
In-Service の優先度制約を満たす場合に `ch_pending` へ加えます。CPU が IRQ6 を受理すると、
最高位の候補を選んで `VR | ch` を返します。候補が無い IACK はスプリアス割り込みとして扱います。

実装済みのチャンネルは、ch15 H-SYNC、ch14 CIRQ、ch13 Timer A、ch12 USART RX、
ch8 Timer B、ch6 V-DISP、ch5 Timer C、ch4 Timer D、ch3 FMIRQ です。未実装要因は
IPR に擬似的な要求を立てません。チャンネル番号が高いものほど優先度が高い。

GPIP 信号の変化とチャンネルの pending は別物です。たとえば CIRQ をポーリングする
ソフトは IER/IMR を無効にしていても、R09 一致時には GPIP6=Low を観測できます。

## 7. 実装上の対応

| 役割 | 主な実装箇所 | 責務 |
| --- | --- | --- |
| MFP 状態 | `src/bus/bus_internal.h` | `mfp_t` とチャンネル定数 |
| MMIO・タイマ・GPIP | `src/peripheral/mfp.c` | レジスタ入出力、tick、信号合成 |
| IRQ/IACK | `src/bus/x68k_bus.c` | IRQ6 判定、優先度選択、受理 |
| CRTC/OPM 連携 | `src/peripheral/crtc.c`、`src/peripheral/opm.c` | ラスター・FMIRQ の信号源 |

## 8. 互換性・既知の制約

- GPIP4/7 は 200 CPU サイクル粒度で量子化され、単発 H-SYNC 待ちが実機より長くなる場合があります。
- Timer A のディレイ・パルス幅測定、Timer B のイベントカウント、TBO→USART クロック配線は未対応。
- GPIP0（RTC ALARM）、GPIP1/2、USART の TX 完了・エラー系チャンネルは未実装です。
- DDR、SCR、UCR は保持しません。TSR は常時 ready を返します。
- USART TX のコマンドは、XEiJ が Timer B=2400bps 相当（`mfpTbKeyboardOn`）のときだけ受理するのに対し、
  ix68k は無条件にデコードする（簡略化）。保持した delay／rate／LED はホスト側へ反映しません。
- USART RX の IPR-A bit4 をゲストが明示的にクリアしたときの `ch_pending` 同期は要検証です。

## 9. テストと受入条件

| 観点 | テスト | 期待結果 |
| --- | --- | --- |
| FMIRQ と IRQ 配送 | `tests/mfp_fmirq_test.c` | マスク／アンマスクと IACK が一致 |
| ISR、VR、EOI | `tests/mfp_isr_eoi_test.c` | S=0/S=1 の優先度とクリア意味論が一致 |
| Timer B | `tests/mfp_timerb_test.c` | 制御、計数、再ロード、ch8 が一致 |
| GPIP ラスター信号 | `tests/mfp_gpip_raster_test.c` | GPIP4/6/7 と CRTC ラスターの整合 |
| スプリアス IACK | `tests/mfp_spurious_iack_test.c` | 候補なし IRQ6 が正しく処理される |
| USART TX | `tests/mfp_usart_tx_test.c` | LED・明るさ・リピートのデコード、境界値、RX 非干渉、RESET 後も保持 |

## 10. 根拠資料

- 一次資料: *Inside X68000* の MC68901、GPIP、タイマ、割り込みベクタの各図。
- 参照実装: XEiJ の `MC68901.java`、`CRTC.java`。

## 11. 更新履歴

| 日付 | 変更内容 |
| --- | --- |
| 2026-09-11 | 現行仕様を Issue ごとの設計記録から分離して新設 |
| 2026-10-31 | USART TX（UDR 書き込み）のキーボードコマンド保持を追加 |
