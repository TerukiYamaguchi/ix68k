# RTC (RP5C15) 実装状況

`src/peripheral/rtc.c`/`rtc.h` + `src/bus/x68k_bus.c`（デコード部）。
IOCS の日付・時刻コール（`_DATESET`/`_TIMESET`/`_DATEGET`/`_TIMEGET`）は
`docs/ix68k-internals/support/iocs_support.md`、設計は `docs/ix68k-internals/design/rtc_design.md`、
アドレスマップ全体は `docs/ix68k-internals/addrmap.md` を参照。

> **必須**: RTC のレジスタ・状態機械を実装・変更したら、このファイルの
> 該当表を必ず更新します。

---

> 2026-09-10 に新規実装、2026-09-12 に
> BANK1・ALARM/GPIP0まで拡張。設計・レビュー経緯は
> `docs/ix68k-internals/design/rtc_design.md` に固定しました。
> 裏取り元は`XEiJ_0260608`の`RP5C15.java`/`MC68901.java`/`MemoryMappedDevice.java`。

## 背景

NetBSD/x68kが起動時にTOD時計を読めず`WARNING: preposterous TOD clock time`等の
警告を出す問題が、`$E8A000`にRTC専用デコードが無くCRTC汎用バイト
配列（常時0初期値）へ落ちていたことが原因と判明。

## アドレスデコード

- `$E8A001`–`$E8BFFF`（奇数のみ）: RTCレジスタ本体。下位5ビットでデコードするため
  `$E8A001`–`$E8A01F`が8KB内に32バイト周期でミラーされます。CRTC全体（`$E80000`–`$E8FFFF`）の
  内側にMFP（`$E88001`–`$E8802F`奇数）と同様の専用デコードとして実装。
- `$E8A000`–`$E8BFFE`（偶数）: バイトアクセスはバスエラー（XEiJ
  `MemoryMappedDevice.MMD_RTC_FIRST/NEXT`のmmdRbs/mmdRbz/mmdWb準拠）。
- ワード/ロングアクセス: 奇数側レジスタのみを反映する（`bus_read_word/long`・
  `bus_write_word/long`に専用分岐、mmdRws/Rwz/Rls・mmdWw/Wl準拠）。偶数側は
  バスエラーにならず単に無視されます。

## レジスタ単位の実装状況

`rtc_reg_read8`/`rtc_reg_write8`（`src/peripheral/rtc.c`）が扱う範囲。
BANK0/BANK1とも`addr&0x1F`で直接引くフラット32バイト配列（XEiJ準拠）。

| レジスタ | アドレス(BANK0時) | 実装状況 |
|---|---|---|
| 1秒 | $E8A001 | ✅ 実装済み（BCD、mask 0x0F） |
| 10秒 | $E8A003 | ✅ 実装済み（BCD、mask 0x07、0-5） |
| 1分 | $E8A005 | ✅ 実装済み（BCD、mask 0x0F） |
| 10分 | $E8A007 | ✅ 実装済み（BCD、mask 0x07、0-5） |
| 1時 | $E8A009 | ✅ 実装済み（BCD、mask 0x0F、24時間制） |
| 10時 | $E8A00B | ✅ 実装済み（BCD、mask 0x03） |
| 曜日 | $E8A00D | ✅ 実装済み（0-6、`weekday_offset`による日付から独立したカウンタ。XEiJ`rtcWeekGap`準拠） |
| 1日 | $E8A00F | ✅ 実装済み（BCD、mask 0x0F） |
| 10日 | $E8A011 | ✅ 実装済み（BCD、mask 0x03） |
| 1月 | $E8A013 | ✅ 実装済み（BCD、mask 0x0F） |
| 10月 | $E8A015 | ✅ 実装済み（BCD、mask 0x01） |
| 1年 | $E8A017 | ✅ 実装済み（BCD、1980起点2桁、Human68k _DATESET等と同じ規約） |
| 10年 | $E8A019 | ✅ 実装済み（BCD、1980起点2桁） |
| MODE | $E8A01B | ✅ bit3=FREE(1)/HOLD(0)、bit2=ALARM出力許可、bit0=BANK選択。有効ビットマスク0x0D（bit1は未定義） |
| TEST | $E8A01D | ✅ 書込み専用。XEiJ互換の選択ビット合成で計時倍率を変え、CPU読出しは0 |
| RESET | $E8A01F | ⚠️ bit3=1Hz、bit2=16Hz出力許可、bit0=ALARMレジスタクリア＋100µs抑止を実装。bit1の分周位相リセットは未実装 |
| BANK1 CLKOUT | $E8A001 | ⚠️ 選択値の保持まで実装（CLKOUT端子の利用者は未実装） |
| BANK1 ADJUST | $E8A003 | ✅ 分以下を0に丸める調整を実装、読出しは0 |
| BANK1 ALARM | $E8A005–$E8A011 | ✅ 書込済み桁だけを現在時刻と比較する7桁比較器を実装 |
| BANK1 12/24H | $E8A015 | ✅ 12/24時間表示変換を実装 |
| BANK1 うるう年カウンタ | $E8A017 | ⚠️ 下位2bitの保持のみ。年送りで加算されず、日付の繰り上がりにも使われない（うるう年判定はホストの`localtime()`/`mktime()`に依存。レジスタ単位の存在監査で判明） |
| BANK1 未使用 | $E8A013/$E8A019 | ✅ 読出し0・書込み無視 |

## HOLD/FREE状態機械

- **FREE**（MODE bit3=1、既定）: 読み出しのたびに`rtc_sync()`が現在時刻（秒が
  変わった時だけ）でBANK0を再構築します。実チップ同様、複数バイトの読み出し中に
  秒が進むと桁上がりが起こりうる。
- **HOLD**（MODE bit3=0）: BANK0を凍結し、読み出しは常に同じスナップショットを
  返します。HOLD中の書き込みは`snap_dirty`フラグを立てるのみで即座には反映しません。
- **FREE→HOLD遷移**: その時点の実時刻でBANK0を再構築して凍結します。
- **HOLD→FREE遷移**: `snap_dirty`なら凍結中の編集内容を時計（`offset_sec`）へ
  書き戻してから自走を再開する（`rtsettod`相当の複数レジスタ書き込みが1回の
  commitにまとまる）。

## 時刻源とIOCSとの統合

- `rp5c15_t.offset_sec`（ゲスト壁時計に加算する秒オフセット）がRTCの時刻源。
  `x68k_bus_rtc_now()`/`x68k_bus_rtc_set()`（`src/bus/x68k_bus.h`）が公開ラッパー。
- MCPデバッガによる停止中はゲスト単調時計と壁時計を凍結します。そのためRTCの
  カウンタ、TESTモードの進行、1Hz/16Hz出力位相も停止し、再開時は停止時間を
  含めず連続して進みます。
- IOCS `_DATESET`/`_TIMESET`($51/$53)・`_DATEGET`/`_TIMEGET`($54/$56)は
  `src/iocs.c`からこのラッパー経由でMMIOと同じ時計を共有する（旧
  `s_rtc_offset_sec`ファイルスコープ変数は廃止）。
- CPU RESET命令（`bus_reset_peripheral()`）ではRTC状態に一切触れない
  （`offset_sec`はもちろんmode/bank/snapshotも保持される）。実チップは背面
  メインスイッチ＋電池でCPU RESET線と無関係に給電され、XEiJ
  `RP5C15.rtcReset()`も空実装であることを確認済み（設計レビューでの指摘事項）。
  初期化は`x68k_bus_create()`の`rtc_init()`一箇所のみ。

## ALARM と MFP GPIP0

- MODE bit2=1かつ、BANK1 ALARMの書込み済み各桁が現在時刻と一致するとALARM出力をLOWにします。
  未書込み桁はワイルドカードとして扱います。
- RESET bit3/bit2で許可した1Hz/16HzのLOW区間もALARM出力へORします。
- `bus_sync()`がALARM線をMFP GPIP0へ反映します。GPIP0は通常HIGH、ALARM時LOWであり、
  AER bit0で選択された遷移時にIERB/IPRB/IMRB bit0を通じてMFP ch0割り込みになります。

## 未実装・要検証

存在監査の結果。レジスタ全16本は参照されています。機能として欠けているのは次の項目。

- うるう年カウンタ（BANK1 $E8A017）が値の保持のみ（上表）。
- CLKOUT選択値（BANK1 $E8A001）が値の保持のみ。TIMER-LED（CLKOUT=Hで点灯、111で消灯）への接続が無い。
- 秒未満の分周カウンタが存在しません。RESET bit1（タイマリセット）が無効で、HOLD中に秒未満の1カウント分を覚えておく挙動もありません。
- 1Hz/16Hz出力の位相がホストの`CLOCK_MONOTONIC`基準で、秒カウンタの桁上がりと同期しません。Inside X68000が述べる「ALARM立ち下がりの96µs後に時刻更新」も再現していません。
- RESETの出力許可ビットの番号が資料間で食い違います。Inside X68000の図9と本文は16Hz=bit4・1Hz=bit3、実装は16Hz=bit2・1Hz=bit3（mask 0x0C、RP5C15データシート側の配置）。XEiJとの突き合わせは未実施。
- RESET bit1の秒未満分周位相リセット。
- CLKOUT選択値を外部端子または表示へ接続する機能。
- TEST倍率・ALARM波形の実機タイミング精度の実機比較。

## テスト

`tests/rtc_rp5c15_test.c`（`rtc_rp5c15_test`）。デコード分離・偶数バスエラー・
ニブル幅マスク・BCD往復・HOLD一貫性・FREE復帰・rtsettod相当の一括書き込み・
年末境界・曜日の独立性・BANK1ミラー/12-24H/ADJUST・ALARM比較器・GPIP0/MFP ch0・
MODE/TEST/RESETレジスタ・peek非侵襲性・ワード/ロングアクセス・IOCSとの時計整合・
CPU RESET非破壊を検証します。

NetBSD/x68k 10.1起動確認済み。
（`scsi_disk0=liveimage-x68k-with-packages-20250106.hds`）で動作検証済み。
`rtc0 at intio0 addr 0xe8a000: RP5C15`でattachし、TOD警告3行
（`preposterous TOD clock time`等）は出ず、rc起動時の`date`出力がホスト実時刻と
一致することを確認済み（2026-09-10）。
