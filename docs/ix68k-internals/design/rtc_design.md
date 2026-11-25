# RTC（RP5C15）設計

## 1. 目的と対象範囲

RP5C15 を `$E8A000` 内の独立した周辺デバイスとして実装し、MMIO と IOCS の日付・時刻
API が同一の時計を参照できるようにします。NetBSD/x68k が RTC を検出し、正しい TOD を得ることを
受入条件とします。

対象は BANK0 の時計、MODE による HOLD/FREE、MMIO デコード、IOCS との時計共有です。
BANK1 の機能、アラーム、計時分周器、ミラーは対象外とします。

## 2. アドレスとアクセス規則

- RTC 本体は `$E8A001–$E8A01F` の奇数アドレスです。CRTC 範囲内だが、MFP と同様に
  バス側で先に専用デコードします。
- 同範囲の偶数アドレスへのバイトアクセスはバスエラーとします。
- ワード／ロングアクセスは奇数側のレジスタ値だけを反映し、偶数側は無視します。
- `$E8A020–$E8AFFF` のミラーは将来の拡張の対象であり、現状は実装しません。

## 3. 時計モデル

唯一の時計源を `rp5c15_t.offset_sec` とし、現在時刻をゲスト時計（`guest_clock_unix_sec()`。通常はホストの `time(NULL)`、デバッガ停止中は凍結）に `offset_sec` を加えて求めます。
MMIO の BANK0 と IOCS `_DATESET`／`_TIMESET`／`_DATEGET`／`_TIMEGET` は、
`x68k_bus_rtc_now()` と `x68k_bus_rtc_set()` を通じてこの時計を共有します。

年月日時分秒は 1980 年起点の2桁年を含む BCD ニブルとして BANK0 へ展開します。曜日は日付から
独立して書き換え可能なカウンタとし、ホスト曜日との差分 `weekday_offset` を保持して、
以後の日送りでも書込値との相対関係を維持します。

## 4. HOLD/FREE 状態機械

| 状態 | 動作 |
| --- | --- |
| FREE（MODE bit3=1） | 読出し前に現在秒との差分があれば BANK0 を再構築します。複数レジスタの読出し途中で秒が進むことを許容します。 |
| HOLD（MODE bit3=0） | BANK0 を凍結し、読出しは同じスナップショットを返します。書込みは BANK0 を編集するが時計へは即時反映しません。 |
| FREE→HOLD | 現在時刻を BANK0 へ展開してから凍結します。 |
| HOLD→FREE | 編集済みなら BANK0 全体を一度だけ時計へコミットしてから自走を再開します。 |

これにより、OS の `rtsettod` 相当の複数バイト書込みが途中状態を時計へ反映しません。

## 5. リセットと未実装機能

CPU RESET は RTC の時計、MODE、BANK、スナップショットを変更しません。RTC は電池で保持される
実機の性質に合わせ、初期化は `x68k_bus_create()` 時の `rtc_init()` に限定します。

- BANK1 は値を保持するだけで、CLKOUT、ADJUST、12/24時間、うるう年カウンタは未実装です。
- MODE bit2 のアラーム出力、TEST、RESET の実機能、MFP GPIP0 駆動は未実装です。
- リセット制御ビットの意味は bit3=1Hz、bit2=16Hz、bit1=秒未満分周段リセット、bit0=ALARMリセットとして記録します。

## 6. 実装と検証

| 役割 | 実装箇所 |
| --- | --- |
| RTC 状態機械・BCD展開 | `src/peripheral/rtc.c` |
| RTC 状態定義 | `src/peripheral/rtc.h` |
| MMIO デコード・IOCS公開ラッパー | `src/bus/x68k_bus.c` |
| IOCS 日付・時刻 API | `src/iocs.c` |

`rtc_rp5c15_test` はデコード、偶数バスエラー、BCD、HOLD/FREE、IOCS 共有、CPU RESET 非破壊を
検証します。NetBSD/x68k は `rtc0 at intio0 addr 0xe8a000: RP5C15` として検出し、TOD 警告を
出さずに起動することを確認します。

## 7. 根拠資料

- RP5C15 データシートと *Inside X68000* の RTC 章。
- XEiJ `RP5C15.java`、`MemoryMappedDevice.java`。

## 8. 更新履歴

| 日付 | 内容 |
| --- | --- |
| 2026-09-10 | 設計・レビュー内容を確定 |
| 2026-09-11 | 設計を本書としてリポジトリへ移管 |
