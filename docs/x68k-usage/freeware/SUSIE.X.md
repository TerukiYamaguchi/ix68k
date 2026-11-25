---
exe: SUSIE.X
種別: freeware
作者: GORRY
版: v1.19A (1997-09-10)
出典: 同梱マニュアル susie.doc（"System of Universal SCSI Integrated Engine"）。配布元 http://retropc.net/x68000/software/disk/scsi/susie/
判別: -（配布アーカイブ sse_121a.lzh 収録版のみ確認）
検証状態: ✅ CD-ROM(CloneCD)経路をix68k検証済み
最終更新: 2026-08-19
---
# SUSIE.X

> 汎用SCSIデバイスドライバ。Human68k純正SCSIDRVを置き換え、HDD・MO・CD-ROM・PD・ZIP等を統一的に扱います。メディア未挿入のドライブにもドライブ名を確保でき、DOSコール経由のメディア交換をリセット無しで認識します。

---

## リファレンス（一次資料要約）

Human68k純正のSCSIDRV系（SCSIHD.SYS・SCSIDRV.SYS等）を置き換えるSCSIデバイスドライバ。
X68000（純正・SxSI）フォーマットに加え、PC-9801・FM-TOWNS・IBM-PC（STANDARD-MO/FDISK）・
ZIP(Iomega)・ISO-9660 CD-ROMのフォーマットも認識します。CD-ROM関連のコールは計測技研
「CDDEV.SYS」と互換になっており、CDDEV.SYS対応ソフト（CDPlayer.x等）もそのまま動きます。

純正SCSIDRVとの一番大きな違いは、**着脱可能メディア（MO等）をメディア未挿入のままでも
ドライブ名として確保できる**点。確保方法は2種類あり、`*:`はパーティション数ぶんの
ドライブを確保、`@:`はメディアの状態に関わらず固定数のドライブを常に確保します。`@:`指定時
はメディアが使えない状態でも指定した数のドライブ名が用意され、ステータス表示のボリューム欄
は`** Not Inserted **`になります。メディアの挿入・交換もDOSコールを通す操作である限り
リセットなしで認識される（監視間隔は`-TIn`/`-TEn`で調整できる）。

### 起動・コマンドラインオプション

書式:

```
SUSIE [ドライブ名] [オプション]
```

CONFIG.SYSからの登録:

```
DEVICE = SUSIE.X -IDn *:
DEVICE = SUSIE.X -IDn @: @: @:
```

`n`は使用するSCSI-ID（0〜7、TWOSCSI使用時0〜9・A〜F）。ドライブ名指定は`*:`または`@:`が
推奨（`A:`〜`Z:`の直接指定もできるが非推奨）。

接続対象を指定するオプション:

| オプション | 意味 | 例 |
|-----------|------|----|
| `-IDn` | 使用するSCSI-ID（0〜7、TWOSCSI時0〜9・A〜F） | `-ID1` |
| `-LUNn` | 使用するLUN（0〜7）。`-`指定でLUN未使用扱い（既定） | `-LUN0` |

常駐後の操作を切り替えるオプション:

| オプション | 意味 | 例 |
|-----------|------|----|
| `-P` | ドライブのステータス表示モードにします。ドライブ名か`-ID`/`-LUN`のどちらかが必要 | `SUSIE E: -P` |
| `-I` | ステータス表示を詳細（使用容量/全容量つき）にする | `SUSIE -P -I` |
| `-R` | 常駐を解除します。ドライブ名か`-ID`/`-LUN`のどちらかが必要 | `SUSIE E: -R` |
| `-K` | 常駐中の既存SCSIDRVを保存したまま切り替え常駐する（解除すると元のドライバに戻る）。ドライブ名は指定できず自動割当になる | `SUSIE -ID0 -K` |
| `-RS` | SCSIバスへリセット信号を送る（機器の動作がおかしいときの救済用） | `-RS` |

動作パラメータを設定するオプション:

| オプション | 意味 | 例 |
|-----------|------|----|
| `-Vn` | ベリファイ方式。`0`=ソフトウェア（既定）、`1`=ハードウェア、`2`=HW書込＋ベリファイ、`3`=なし | `-V1` |
| `-En` | `1`で内部的にイジェクト状態にする（アクセス不可になる）。既定`0` | `-E1` |
| `-Wn` | `1`で内部的にライトプロテクト状態にします。既定`0` | `-W1` |
| `-Bn` | `1`でBUSY（メディア入れ替え中等）状態を待ちます。`0`だと即エラーにします。既定`0` | `-B1` |
| `-Hn` | `1`で高速認識モードにする（初期化コマンドの一部を省略。機器によっては非対応）。既定`0` | `-H1` |
| `-Un` | `1`でSCSI-ID未接続・認識不能でも常駐を続行します。既定は`DEVICE=`登録時`0`、コマンドライン常駐時`1` | `-U1` |
| `-RBn` | `1`でCD-ROM上のMacリソースファイル（先頭`#`）へのアクセスを許可します。既定`0` | `-RB1` |
| `-AAn` | `1`でCD-ROMのオーディオ再生コマンドをCDDEV.SYS非互換にします。既定`0`（互換） | `-AA1` |
| `-TIn` | メディア挿入の監視間隔（n/100秒）。既定`100`（1秒） | `-TI50` |
| `-TEn` | メディアイジェクトの監視間隔（n/100秒）。既定`100`（1秒） | `-TE50` |
| `-TWn` | アクセス再試行までの待ち時間（n/100秒）。既定`20`（0.2秒） | `-TW40` |
| `-Cn` | 内部バッファのサイズ（2〜32）。ソフトウェアベリファイやCD-ROMキャッシュに使います。既定はHDD/MO=`8`、CD-ROM=`16` | `-C2` |
| `-EXn` | `1`でREAD/WRITE拡張コマンド（READEXT/WRITEEXT）を使います。既定`1`。非対応機種では内部的に`0`扱いになる | `-EX0` |

表示や特殊用途のオプション:

| オプション | 意味 | 例 |
|-----------|------|----|
| `-SI` | ドライブ情報メッセージの表示を抑える | `-SI` |
| `-F` | （テスト用）常駐中のSUSIEからの切替常駐を許可する | - |
| `-Z` | （テスト用）一部の危険な動作を許可します。誤用するとメディアを破壊する恐れがあり非推奨 | - |

### 用途別の使い方

| 用途 | コマンド例 |
|------|-----------|
| ドライブを調べる（切替可否の確認） | `SUSIE` |
| CONFIG.SYSで通常のSCSIDRVとして常駐 | `DEVICE = SUSIE.X -IDn *:` |
| コマンドラインから新規常駐 | `SUSIE -IDn [ドライブ名]` |
| 常駐済みSCSIDRVから切替常駐 | `SUSIE -IDn -K`（`[SUSIE recognizable drive]`欄に出るドライブのみ可能） |
| ステータス確認・状態変更 | `SUSIE [ドライブ名] -P [状態変更オプション]`（`-P`は省略可） |
| 常駐解除 | `SUSIE [ドライブ名] -R` |

引数なしで`SUSIE`を実行すると、切替常駐（`-K`）できるドライブが`[SUSIE recognizable
drive]`欄に、できないドライブ（FDD・RAMDISK等）が`[SUSIE non-recognizable drive]`欄に
一覧表示されます。

### ステータス表示

```
Drv Driver       Device Name           [RP] (VEWBH) -TI -TE -TW  Size   Volume
 S: *SUSIE*(X68) SCSI MO    (ID4  ,U0) [RP] (V---H) 100 100  20  121M S:ARCPOOL
 T: *SUSIE*(X68) SCSI MO    (ID4  ,U1) [RP] (V---H) 100 100  20 ----- T:** Not Inserted **
```

| 項目 | 内容 |
|------|------|
| Drv | Human68k上のドライブ名 |
| Driver | 認識しているメディアフォーマットの種類: X68(X68000)/NEC(PC-9801)/FMT(FM-TOWNS)/IBM(IBM-PC STANDARD-MO)/FDI(IBM-PC FDISK)/ZIP(Iomega)/IS9(ISO-9660 CD-ROM) |
| Device Name | デバイス種別とSCSI-ID・LUN・ユニット番号（LUNが0か認識不能なら非表示） |
| [RP] | R=着脱可能(Removable)、P=ライトプロテクト中。該当する場合のみ表示 |
| (VEWBH) | `-V`/`-E`/`-W`/`-B`/`-H`の各オプションが有効なとき対応する文字が立つ |
| -TI/-TE/-TW | 各オプションの現在値 |
| Size | ディスクの全容量 |
| Volume | ボリュームラベル。`(No Volume Name)`＝ラベルなし、`** Not Inserted **`＝メディア未挿入、`** Unrecognized **`＝フォーマット未認識、`** Audio Only **`＝音楽CD、`** Unconnected **`＝SCSI機器を認識できない |

`-I`オプションを付けると`Size`/`Volume`欄の代わりに`Used / Capacity`（KB単位、使用率%）が
表示されます。

### 添付ツール

| ファイル | 機能 |
|---------|------|
| `deej.x` | 常駐し、電源断（POWER-OFF）時に全デバイスのメディアを排出します。`-CD`オプションでCD-ROMも対象に含める |
| `SSLUN.x` | 常駐中のSUSIEドライブのLUNを切り替える（多連装CD-ROMを1つのドライブ名で使い分ける用途）。例: `SSLUN E: 1` |

### 拡張構成

- **TWOSCSI併用**: TNB製作所「TWOSCSI」常駐時、2ポート目のSCSI-IDとして8〜15（表記上16進数8〜9・A〜F）が使えるようになります。SUSIE自体を2つのTWOSCSI登録行の間に置く必要はなく、末尾（`DEVICE=SUSIE.X -ID9 @:`のような形）で構いません。
- **LS-120**: TNB製作所「FDS120P」「GOVERHD」常駐時、I/O DATA「FDS-120」のLS-120ドライブが使えるようになります。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/disk/scsi/susie/ から`sse_121a.lzh`としてダウンロード可能です。

### MCP アクション

```
run_command("SUSIE")                # 既存の登録済みドライブ一覧・切替可否確認
                                     # （まだ未接続のSCSI IDはここには出ない）
run_command("SUSIE -ID2 *:")        # ID2にコマンドラインから新規常駐（CD-ROM等）
run_command("SUSIE E: -P")          # E:のステータス表示（Driver欄・Volume欄を確認）
run_command("SUSIE -P -I")          # 全ドライブの詳細ステータス（使用量/全容量つき）
run_command("SUSIE E: -R")          # E:の常駐解除
disk(action="insert", media="scsi", index=2, path="/path/to/image.ccd")
                                     # CD-ROM(CloneCD)イメージの挿入。拡張子.ccdで
                                     # 自動判別（media="scsi"のまま、専用引数は無い）
```

CONFIG.SYSで`DEVICE = \SYS\SUSIE.X -ID<n> *:`を常駐登録した場合、対象IDに
メディア未挿入でもドライブ名が確保される（ステータス上`** Not Inserted **`）。
その状態でMCPの`disk(action="insert", ...)`を呼ぶとreset無しでメディアが
認識されます。

### 検証メモ

- 検証: ✅ CD-ROM(CloneCD)経路をix68k検証済み（対象イメージ
  `disk/Gekkan Dennou Club Perfect Collection Vol. 1-50 [CD]/`）。
- 出典アーカイブ: `sse_121a.lzh`（本文中の逆アセンブル資料ディレクトリ内に展開済み、`susie.doc`/`susie.x`/`SOURCE/`等を同梱）。
- コマンドライン新規常駐（`SUSIE -ID2 *:`）: `E: *SUSIE*(IS9) SCSI CD-ROM(ID2,U0)
  [RP] ... 203M E:MKCDP0001`と表示され、`DIR E:`でISO-9660のディレクトリ/
  ファイル一覧（名前・サイズ・タイムスタンプ）が正しく読めることを確認。
- CONFIG.SYS常駐（`DEVICE = \SYS\SUSIE.X -ID2 *:`）: 冷起動でメディア未挿入の
  `D:`が確保され、起動後にMCPでイメージを挿入するとreset無しで認識、
  `DIR D:`/`TREE D:`/`COPY`（対象CloneCDからのファイルコピー）が正常動作しました。
- HDD(SCHDISK)経路もSUSIEへ切替常駐できることを`SUSIE`引数なし実行の
  `[SUSIE recognizable drive]`欄（`C: SCHDISK(---) SCSI HDD(ID0,U0) ...`）で確認済み
  （ただし切替常駐自体（`-K`）は未実施、HDD経路の検証はCD-ROM対応のスコープ外）。
- MO(`-ID1`)経路は未検証（`CONFIG.SYS`にコメントアウト行`;DEVICE = \SYS\SUSIE.X
  -ID1 *:`が残置されているが、CD-ROM経路のみを検証したため変更していない）。
