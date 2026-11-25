# ブート動作（現在の状態）

- IPLROMのリセットベクタからMC68000を起動します。
- フロッピーディスクからHuman68k 3.02を冷起動できます。
- 内蔵SCSI BIOS ROMとSPC/DMAC ch1を経由し、SCSI HDDからHuman68kを起動できます。
- RTC（RP5C15）を検出するOSを含めて起動できます。NetBSD/x68k 10.1ではTOD警告なしで
  起動することを確認済み。
- MCPからキー入力、画面取得、FD/SCSIメディアの挿入・排出、ライトプロテクト変更、
  リセットを操作できます。

## ROMとSRAM

IPLROMとCGROMは起動時に外部ファイルから読み込みます。内蔵SCSI BIOSは
`src/peripheral/scsi_rom_data.h` の8KB ROMを `$FC0000-$FC1FFF` に配置し、
`$FC2000-$FDFFFF` は `0xFF` を返します。旧実装の6バイトRTSスタブは使用していません。

SRAMは `sram.dat` に永続化します。新規作成時は `src/bus/sram.c` が実機ROMの既定値に
相当する内容を設定し、SCSI BIOSエントリ `$FC0000` を含む起動設定を用意します。
既存のSRAM設定は保持されます。起動デバイスやメモリ容量をオフラインで変更する場合は
`tools-host/sram_switch/` を使います。

RTC はゲスト時計（`src/bus/guest_clock.c`、ホスト実時刻をデバッガ停止中は凍結する）とオフセットを時刻源として保持し、MMIO とIOCSの日付・時刻APIで共有します。
CPU RESET ではRTCの時計・MODE・BANKを初期化しません。詳細は
[rtc_design.md](design/rtc_design.md) と [rtc_support.md](support/rtc_support.md) を参照。

## フロッピーブート

`-fd0`または設定ファイルの`fd0`でブートディスクを接続します。FDCコマンドとDMAC ch0を
経由してIPLROMのディスク処理を実行し、IOCS `_B_READ`/`_B_WRITE`もROMへ委譲します。
標準2HDはヘッダなしイメージの `.xdf`、`.hdm`、`.2hd`。9SCSET系などの別媒体も拡張子で判別して扱えます（一覧は [fdc_support.md](support/fdc_support.md) を参照）。

`-boot fdd` は SRAM `$ED0018` を `$0000` に設定し、FDD優先を明示します。設定ファイルの
接続指定（`fd0` 等）とは独立しているため、起動するには媒体も接続する必要があります。

## SCSI HDDブート

`-scsi0`〜`-scsi6`または設定ファイルの`scsi_disk0`〜`scsi_disk6`でHDSイメージを接続します。
読み取り専用の固定HDDをゲストから書き換え可能にし、かつデバッガのメモリ内
スナップショットを使う場合は、同じIDに `scsi_overlayN` で差分ファイルを明示指定します。
書き込みはbase HDSへは行わず、差分ファイルへ記録されます。差分ファイルはHDSごとに新規作成し、
複数プロセスから共有しません。

```ini
scsi_disk0 = disk/netbsd.hds
scsi_overlay0 = disk/netbsd.cow
```

ブート経路は次の通りです。

```text
IPLROM → $FC0000 内蔵SCSI BIOS → SPC(MB89352) → DMAC ch1 → HDSイメージ
```

SPCは実レジスタ経路でコマンドを処理し、Read/Write(6/10)、Inquiry、Mode Sense、
Read Capacity、Format Unitなど、Human68kの起動・運用に必要なコマンドを実装しています。
`-boot scsi` は SRAM `$ED0018` を `$A000` に設定してSCSI優先を明示します。起動可能な
HDSイメージの作成手順は `docs/x68k-usage/mcp/provisioning/hdd_install.md`、レジスタ・メディア仕様は
[scsi_support.md](support/scsi_support.md) を参照。

設定値 `MO`/`MODRV` は空のリムーバブルドライブ、`CD` は空のCD-ROMドライブを接続します。
いずれも媒体未挿入の状態で起動し、後から媒体を挿入できます。

## リセットと一時的なFD0起動

通常のリセットはCPUベクタとリセット対象の周辺機器を初期化するが、`sram.dat` とRTCは保持します。
MCPの `RESET FD0` と対応するホットキーは、実機の「OPT.1を押しながらリセット」に相当します。
リセット直後だけ SRAM `$ED0018` を `$0000` としてROMに読ませ、実読出しを検出すると元の
起動優先値へ復元します。固定の待ち時間ではないため、CPU速度やホスト負荷に依存しません。

## IOCSの扱い

ディスク、画面、音源、キーボードなど、実装済みハードウェアを使えるIOCSは原則として
IPLROMへ委譲します。C側で処理する互換ハンドラと、未対応機能の安全なダミー応答は
`src/iocs.c`に集約しています。関数単位の状態は [iocs_support.md](support/iocs_support.md) を参照。
