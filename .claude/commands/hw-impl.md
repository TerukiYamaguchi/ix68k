# 周辺機器 実装状態

`src/peripheral/` + `src/bus/x68k_bus.c` に各周辺チップを実装している。
実装状況は**ペリフェラル単位で`docs/develop/support/*.md`に分割**されている
（2026-07-29、Issue #78の全周辺機器監査を機に`docs/hw_support.md`から分割）。

> **必須**: レジスタ・コマンドを実装・変更したら該当する
> **`docs/develop/support/<peripheral>_support.md`** を必ず更新する
> （レジスタ表・コマンド表・既知の未実装項目の一覧）。

## 概要・詳細ファイル対応

- FDC (µPD72065B): `$E94000–$E95FFF`。奇数アドレスのみ実アクセス。READ/WRITE DATA
  等主要コマンドは実装済み、READ TRACK/FORMAT TRACK/SCAN系/STANDBY系等が未実装
  → 詳細 `docs/develop/support/fdc_support.md`
- MFP (MC68901): USART(キーボード)・Timer A/C/D・ラスタ一致(GPIP6)・H-SYNC(GPIP7)
  は実装済み。**Timer B・GPIP4(V-DISP、Issue #77)・USART TX・GPIP0-2系は未実装**
  → 詳細 `docs/develop/support/mfp_support.md`
- SCSI (SPC/MB89352) → `docs/develop/support/scsi_support.md`
- DMAC (HD63450) → `docs/develop/support/dmac_support.md`
- OPM (YM2151) → `docs/develop/support/opm_support.md`
- ADPCM (MSM6258V) → `docs/develop/support/adpcm_support.md`
- CRTC/ビデオコントローラ → `docs/develop/support/crtc_support.md`
- スプライトコントローラ → `docs/develop/support/sprite_support.md`
- FPU-CIR (CZ-6BP1/MC68881、XB1 `$E9E000`、既定無効) → `docs/develop/support/fpu_cir_support.md`
- 割り込み系統（CPU IPL別、全ペリフェラル横断） → `docs/develop/addrmap.md`
