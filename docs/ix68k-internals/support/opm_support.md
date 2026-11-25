# OPM (YM2151) 実装状況

関連: 現行設計は `docs/ix68k-internals/design/opm_design.md`、MFP GPIP3(FMIRQ)配線は `docs/ix68k-internals/support/mfp_support.md`、
IOCS は `docs/ix68k-internals/support/iocs_support.md` を参照。

> **必須**: OPM のレジスタ・機能を実装・変更したら、このファイルの該当表を必ず更新します。

---

FM音源(OPM/YM2151)実装に伴い新設。`src/audio/ym2151.c`（合成コア）、
`src/audio/ym2151_timer.c`（タイマ進行、いずれもymfm移植・バス/CPU非依存）+
`src/peripheral/opm.c`（X68000グルー層）で構成。詳細な
現行設計は`docs/ix68k-internals/design/opm_design.md`を参照。

**全レジスタ存在監査（2026-07-29）**: YM2151全レジスタ（$20-$FFのチャンネル/オペレータ
パラメータ群含む）は`ym2151.c`が著名なオープンソースコア`ymfm`の移植のため、
MFP/FDC等の自前実装と異なり「存在すら知らないレジスタ」的なギャップは想定しづらい。
既知の未実装（CSMキーオントリガ・YM3012 DAC量子化再現）は既に下表に記載済みで、
今回の監査で新規のレジスタ単位ギャップは見つからありませんでした。

### レジスタ・機能単位の実装状況

| 機能 | 実装状況 |
|---|---|
| アドレス配置($E90001選択/$E90003データ) | ✅ 実装済み(`opm_reg_write8`) |
| ステータスレジスタ(bit7=BUSY, bit1=TimerB, bit0=TimerA) | ✅ 実装済み(`opm_status_read8`)。BUSYは時刻ベース(64クロック、REG_MODE書き込みは対象外) |
| 波形合成(PG/EG/LFO/ノイズ、CON=0-7) | ✅ 実装済み(`ym2151_generate`、62500Hz固定・ステレオint32・中間クリップ無し) |
| L/Rパン(チャンネルレジスタ$20-$27 bit6/7) | ✅ 実装済み。ハードスイッチ(クロスフェード無し) |
| OPM内蔵タイマA/B | ✅ 実装済み(`ym2151_run_timers`)。LOAD(bit0/1)はエッジトリガのstart/stop、IRQEN(bit2/3)はフラグセットのゲート(既存フラグのマスクではない) |
| CSM(レジスタ$14 bit7)キーオントリガ | ❌ 未実装。詳細な発音・解放タイミングが一次資料/ymfm本家から未確定のため保留 |
| YM3012 DAC量子化再現(`roundtrip_fp`) | ❌ 未実装(恒等スタブ)。音質精度追求時のみ対象 |
| MFP GPIP3(FMIRQ)配線 | ✅ 実装済み(`x68k_bus_mfp_opmirq`、Timer C割り込みパスと同型) |
| ADPCMとのミキシング | ✅ 実装済み(`audio_mix_sum_clip_stereo`、int32中間+最終段1箇所クリップ) |
| IOCS `_OPMSET`(0x68)/`_OPMSNS`(0x69)/`_OPMDRV`(0xF0) | ✅ ROM委譲済み |
| IOCS `_OPMINTST`(0x6A) | ✅ ROM委譲済み |

### 実装上の注意点

- **CT1/CT2(レジスタ$1B)は二重管理**: `src/peripheral/adpcm.c`の`adpcm_opm_write8`が
  ADPCMクロック切替・FDC READY強制用に独立して追跡し続けます。OPM合成コア側の
  レジスタファイル($1B含む全レジスタ)とは別に保持している（両方とも同じ生バイトを
  受け取るのみで、相互に参照しない）。
- **OPM内蔵Timer A/B と MFP内蔵Timer A/B/D は完全に別チップの別機能**。混同しないこと。
  いずれも実装済みだが、用途・クロック・割り込み経路は独立しています。
- 実機の垂直帰線期間中のみレジスタ更新有効等のタイミング精度は未実装です。


## 変更履歴

| 日付 | 内容 |
|------|------|
| 2026-07-21 | FM音源(OPM/YM2151)を実装。セクション新設しレジスタ・機能単位の実装状況を記載 |
| 2026-07-29 | `docs/hw_support.md`から本ファイルへ分離（ペリフェラル単位でのファイル分割） |
