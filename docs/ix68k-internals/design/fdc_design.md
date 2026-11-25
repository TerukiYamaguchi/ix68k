# FDC 設計

## 1. 目的と対象範囲

ix68k は µPD72065B 互換の FDC を、X68000 の標準 2HD フロッピーを扱うための
必要範囲で実装します。本書は FDC の現行仕様を定めます。

- 対象: MMIO、コマンド／結果フェーズ、XDF バッキング、DMAC ch0 転送、割り込み、
  DDAM、ライトプロテクト、リセット、イジェクト。
- 対象外: アドレスマーク・CRC・ギャップを含む磁気媒体の物理表現、任意ジオメトリ、
  D88 などのヘッダ付きイメージ、未実装コマンドの設計経緯。
- 仕様の一覧: レジスタ・コマンドごとの対応状況は
  [`fdc_support.md`](../support/fdc_support.md) を正本とします。

## 2. 実装状態

| 項目 | 状態 | 補足 |
| --- | --- | --- |
| 標準 2HD XDF | ✅ | 77 cylinder × 2 head × 8 sector × 1024 byte |
| READ/WRITE DATA | ✅ | DMAC ch0 経由の 1 セクタ転送 |
| READ/WRITE DELETED DATA | ✅ | セクタ単位 DDAM をメモリ上で保持 |
| SCAN EQUAL | ✅ | `_B_VERIFY` 用、`0xFF` はドントケア、結果は ST2 SH/SN |
| SEEK／RECALIBRATE／SENSE 系 | ✅ | 完了時に IOI IRQ1 を要求 |
| IOCS `_B_FORMAT` | ✅ | FDC opcode を通さない標準 2HD の直接初期化 |
| READ TRACK／FORMAT TRACK | ❌ | FDC ポート直叩きの opcode 0x02／0x0D は未実装 |
| 複数セクタ・複数トラック転送 | ❌ | IOCS が発行する EOT=R の単一セクタ形のみ |

## 3. 設計の要点

1. **媒体はフラットな XDF バイト列です。** FDC は物理トラックを再現せず、標準
   2HD の C/H/R/N を XDF 内の固定オフセットへ変換します。
2. **通常の転送は DMAC ch0 が担います。** FDC は実行フェーズの転送方向・残量・対象位置を
   用意し、DMAC が `$E94003` を介して消費します。
3. **DDAM と WP はイメージ形式に埋め込みません。** DDAM はドライブごとのビットマップ、
   WP は `access(path, W_OK)` から得る状態であり、どちらも挿入／排出単位で扱います。
4. **IOCS と FDC opcode は別レイヤです。** `_B_FORMAT` は実機ROMへ委譲され、ROMが
   FDC の FORMAT TRACK を発行します。XDF の初期化は `fdc_format_complete()` が行います。

## 4. 構成とデータモデル

`fdc_t` がフェーズ、コマンド／結果 FIFO、ドライブごとのヘッド位置と媒体、実行
データフェーズを保持します。FDC は `x68k_bus_t` に属し、バス層が MMIO・媒体の挿抜・
IOI 信号を仲介します。

| 状態 | 内容 |
| --- | --- |
| `phase` | `FDC_IDLE`、`FDC_CMD`、`FDC_EXEC`、`FDC_RESULT` |
| `cmd[]` / `res[]` | コマンド受信と結果返却の FIFO |
| `cur_cyl[4]` | ドライブごとの機械的なヘッド位置 |
| `disk[]` / `disk_path[]` | XDF のメモリバッファと、書戻し先のホストパス |
| `dma_*` | 実行中の転送先、方向、残量、書戻し範囲 |
| `ddam[4][]` | 削除データアドレスマークのセクタ単位ビットマップ |
| `write_protect[4]` | ホスト側の書込み権限から導出した状態 |

標準 2HD のオフセットは次で求めます。`N=3` のセクタサイズは 1024 byte です。

```
offset = ((cylinder * 2 + head) * 8 + (sector - 1)) * (128 << N)
```

## 5. インターフェース／レジスタ／入力

µPD72065B は下位 8 bit 接続で、チップ内部 A0 はバス A1 に接続されます。そのため
`$E94000–$E95FFF` のうち奇数アドレスだけが実アクセスです。

| アドレス | 機能 | ix68k の仕様 |
| --- | --- | --- |
| `$E94001` | MSR | IDLE=`0x80`、コマンド=`0x90`、結果=`0xD0` |
| `$E94001` / `$E94003` | Data write | コマンド、または実行フェーズの書込みデータを受理 |
| `$E94003` | Data read | 結果、または実行フェーズの読出しデータを返す |
| `$E94005` | Drive Status / Control | 読出しは DISK IN、書込みは対象選択と EJECT 制御 |
| `$E94007` | Drive Select | 書込みを保持。読出しは未実装で `0xFF`。アクセスランプ用の選択・motor状態はCPU RESETでクリア。bit4（1=2DD/2D=250kbps、0=2HD=500kbps）はFDCの転送速度として保持し、媒体プロファイルの転送速度と照合する（§8） |

FDC unit は内部 drive index と同一で、0／1 は内蔵 FD0／FD1、2／3 は外付け FD2／FD3
に対応します。

## 6. 動作仕様

### 6.1 コマンドと転送

READ DATA (0x06)、WRITE DATA (0x05)、WRITE DELETED DATA (0x09)、READ DELETED
DATA (0x0C)、SCAN EQUAL (0x11) は、コマンド受信後に対象 C/H/R/N を検証します。
未挿入や範囲外では、データフェーズを開始せず ST0/ST1 を伴う結果フェーズへ移る。

有効な READ／WRITE は `FDC_EXEC` を開始し、通常は
`dmac_ch0_fdc_transfer()` が DMAC ch0 の設定に従って全バイトを転送します。CCR START が
ない異常系だけ、FDC 内のフォールバックが Data Register 経由で転送を消化します。
転送完了後に結果 FIFO と FDC IRQ を確定します。

WRITE DATA は対象セクタの DDAM を消去し、WRITE DELETED DATA は設定します。READ DATA
で DDAM セクタを読む場合、または READ DELETED DATA で通常セクタを読む場合は、ST2 の
CM を立てます。SCAN EQUAL はイメージを書き換えず、ホストから届く `0xFF` をドントケア
として比較します。SCAN の結果は ST2 bit3 の SH（一致）または bit2 の SN（不一致）であり、
IOCS `_B_VERIFY` の戻り値は成功時も生ステータス値のため非ゼロになり得ます。

SCAN、READ／WRITE、DELETED DATA は、標準2HDジオメトリをポインタ演算前に検証します。
IOCS が発行する SK=0・EOT=R の単一セクタ形だけを対象とし、DMAC非武装のフォールバックで
MARが範囲外なら、偽の成功ではなく異常終了にします。

### 6.2 媒体の挿抜とライトプロテクト

受理するイメージは、ヘッダなし標準 2HD の `.xdf`、`.hdm`、`.2hd` のみです。
挿入時に DDAM を全消去し、`access(path, W_OK)` によるホストファイルの実効書込み可否から
WP を設定します。排出時にも DDAM と WP を消去します。DDAM は XDF へ書き戻さないため、
再挿入・再起動で失われます。

WP 中の WRITE DATA／WRITE DELETED DATA は、Not Ready の直後かつジオメトリ検証より前に、
DMA・DDAM 更新・ファイル書戻しを行わず ST0 abnormal termination と ST1 bit1 の NW を返します。
SENSE DRIVE STATUS は、媒体が挿入されている場合だけ ST3 bit6 で WP を返します。`$E94005` に
WP ビットは存在しません。`_B_FORMAT` の直接初期化にも同じ WP ガードを適用します。

GUI／MCP の protect・unprotect は `x68k_bus_fdc_set_write_protect()` を通じてホストファイルの
owner write bit を変更し、同一ファイルを挿入している全ドライブの WP 状態を再判定します。
外部から属性を変更した場合は、再挿入またはこのトグルまで内部状態へ反映されません。

### 6.3 フォーマットとイジェクト

`_B_FORMAT` は実機ROMへ委譲され、ROMが発行する FDC FORMAT TRACK が、指定トラックを
フィルバイトで初期化して当該トラックの DDAM を消去します。結果の D0 形式への変換は ROM が行います。
Not Ready → WP → ID 照合(ND)の順に FDC 側で判定します。FORMAT.X が標準 77 cylinder の
直後を指定する場合は、ユーザー可視の完了を保つため成功 no-op とします。

ドライブ制御レジスタの EJECT は選択ビットの fall edge で実行します。GUI 等のボタン
経由のイジェクトは、マスク中または FDC が idle でない間は無視します。

### 6.4 割り込みとリセット

RECALIBRATE、SEEK、データ転送の完了は `fdc.irq` を立て、IOI の FDC 信号を通じて
CPU の IRQ1 へ反映します。MFP GPIP は使用しません。

CPU RESET と電源投入時は `fdc_reset()` がµPD72065内部のフェーズ、FIFO、実行転送状態、IRQを
初期化します。媒体、DDAM、WP、現在シリンダは保持します。SENSE INTERRUPT STATUS の割込み無し状態は
ST0=`0x80` とします。CPU RESETでは、ゲートアレイ側の `$E94005` 前回書込みラッチ
`drv_ctrl_latch` と `$E94007` のアクセスランプ用 `fdc_drive_sel` / `fdc_motor_on` もクリアします。
これによりDrive Statusは選択なしの0となり、RESETをまたぐ選択bitのfall edgeによる意図しない
排出を防ぎます。書込みハンドラの副作用を避けるため直接代入します。一方、物理イジェクト禁止の確定状態
`fdc_eject_masked_bits` と、抜き挿し通知 `fdc_ejected_flag[]` は保持します。

## 7. 実装上の対応

| 役割 | 主な実装箇所 | 責務 |
| --- | --- | --- |
| FDC 状態とコマンド | `src/peripheral/fdc.h`, `src/peripheral/fdc.c` | フェーズ、応答、DDAM、実行データフェーズ |
| DMAC ch0 転送 | `src/peripheral/dmac.c` | FDC Data Register と RAM の転送 |
| MMIO・媒体操作・IOI | `src/bus/x68k_bus.c` | レジスタ、挿抜、WP、IRQ 信号、直接フォーマット |
| IOCS バイパス処理 | `src/iocs.c` | `_B_FORMAT` の標準 2HD 初期化 |
| UI／MCP 操作 | `src/ui/disk_window.c`, `tools-host/mcp/` | 挿抜と WP 状態の操作・表示 |

## 8. 互換性・既知の制約

- READ TRACK (0x02) と FORMAT TRACK / WRITE ID (0x0D) の FDC opcode は未実装です。
- READ／WRITE／DELETED DATA は単一セクタ形のみで、SK、MT、複数セクタ EOT は未対応。
- 物理的な DAM、DDAM、CRC、ギャップ、インターリーブは XDF に表現しません。
- Drive Select の読出しは未実装です。
- 転送速度: `$E94007` bit4 が示す転送速度（250/500kbps）が挿入媒体のプロファイルと
  一致しない場合、READ ID は MA（ST1 bit0）、READ／WRITE／SCAN／FORMAT TRACK は ND で失敗します。
  X68030 Compact 相当の、2DD も扱えるドライブを常に仮定する（設定項目なし）。RESET で 500kbps に戻る。
  これにより 9SCDRV.X は 2DD/9・2DD/8 を 2HC と誤判定せず、2DD モードへ進みます。
- 回転数: `$E94005` bit3-0（Option Select）の最終書込値を保持し、ドライブ N が N と
  N+1 mod 4 の同時アサートのときだけ 300rpm、それ以外は 360rpm とする（9SCHRD.DOC）。媒体プロファイルの
  `rpm300`（2HQ のみ 1）と照合するのは、500kbps 媒体の READ／WRITE／SCAN だけで、不一致は ND。
  READ ID、FORMAT TRACK、250kbps 媒体（2DD）は回転数を問いません。RESET で 360rpm に戻る。
- READ ID の巡回: READ ID はドライブごとのカーソルでトラックのセクタを順に返し（R=1、2、…、
  最終セクタ、先頭へ）、RESET で先頭に戻る。9SCDRV.X の既定（`MedChk :R-ID`）は READ ID を 21 回呼んで
  得た R の最大値でセクタ数を判定する（18 以上は 2HQ、15 は 2HC）。常に先頭セクタを返す実装では
  2HQ の判定が収束せず `DIR` がメディアエラーで止まいました。
- 9SCDRV.X の `/M` 相当（READ DATA のみで判定する経路。`L000185` が 0）は、360rpm/500k の
  R=1 N=3 → 300rpm/500k の R=1 N=2（成功なら 2HQ）→ 300rpm/250k の R=2 N=2（2DD/9）→
  360rpm/500k の R=1 N=2（成功なら 2HC）の順に READ DATA を試します。この経路は回転数の照合に依存します。
  逆アセンブル（`dis`）で読んだのみで、エミュレータ上では未確認です。
- root 権限ではホストの `access(W_OK)` が読み取り専用媒体を WP と判定できない場合があります。
- SASI/SCSI のライトプロテクトは FDD と異なる機構であり、本設計の対象外です。

## 9. テストと受入条件

| 観点 | テスト | 期待結果 |
| --- | --- | --- |
| ドライブ・Ready・割込み | `tests/fdc_probe_test.c`, `tests/fdc_status_test.c` | ユニット対応、MSR、DMAC 武装が一致 |
| 削除データ | `tests/fdc_deleted_test.c` | DDAM と CM がコマンド種別どおり |
| SCAN | `tests/fdc_scan_test.c` | 比較・ドントケア・非書込みが一致 |
| ライトプロテクト | `tests/fdc_wp_test.c` | ST3/WP と書込み拒否が一致 |
| RESET | `tests/fdc_reset_test.c` | 実行状態・FDD制御ラッチと IRQ を消去し、媒体・ヘッド位置・物理イジェクト禁止状態を保持 |
| イジェクト・直接フォーマット | `tests/fdc_eject_test.c` | DDAM 消去、範囲外 no-op、WP が一致 |
| 転送速度の照合 | `tests/fdc_rate_test.c` | 全プロファイルで、転送速度が一致するときだけ READ ID／READ DATA／FORMAT TRACK が成功 |

## 10. 根拠資料

- 一次資料: *Inside X68000* の FDD インタフェース、コマンド、ステータス、フォーマット章。
- 一次資料: µPD72065B データシート。
- 参照実装: XEiJ `FDC.java`、`FDMedia.java`。
- 実装状態: [`fdc_support.md`](../support/fdc_support.md)。

## 11. 更新履歴

| 日付 | 変更内容 |
| --- | --- |
| 2026-10-10 | `$E94005` bit3-0 の回転数（360/300rpm）をモデル化し、500kbps 媒体の READ／WRITE／SCAN だけ回転数を照合（2HQ=300rpm）。READ ID をセクタ順の巡回に変更（9SCDRV.X が 2HQ を判定できるように）。 |
| 2026-10-10 | `$E94007` bit4 の転送速度を媒体プロファイルと照合（2DD を 2HC と誤判定する問題の修正）。回転数は非対応。 |
| 2026-09-13 | CPU RESETでPEDEC側のDrive Control前回値ラッチとDrive Select由来のアクセスランプ状態をクリアし、リセットをまたぐ意図しない排出を防止。物理イジェクト禁止状態と抜き挿し通知は保持。 |
| 2026-09-11 | 現行の FDC 仕様を新設 |
