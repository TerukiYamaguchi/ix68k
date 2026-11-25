# hu68disk

バージョン 1.0（`hu68disk --version` で表示）。

Human68k (Sharp X68000) ディスクイメージのファイル操作ツール（C99 / macOS・Linux・Windows 対応。依存は iconv のみで、Windows は Win32 API を使う）。

`mtools` はホスト側で長名ファイルをコピーすると **VFAT ロングファイル名エントリ**
（属性 `0x0F`・UTF-16・NUL バイト）を生成し、Human68k がそれを不正メディアとして
拒否する（本リポジトリ Issue #30 の真因）。`hu68disk` は Human68k **ネイティブ**の
長ファイル名を正しく書き込むため、この問題が起きない。

- **Human68k ネイティブ長名**: 名前 1〜8 文字目を `+0x00`、9〜18 文字目を予約領域
  `+0x0C`〜`+0x15` に地続き格納（最大 **18 バイト + 拡張子 3 バイト = 21 バイト**）。
  VFAT のような別ダミーエントリは一切作らない。
- **Shift-JIS**: ディスク上の名前は Shift-JIS。ホスト（macOS/Linux 端末＝UTF-8）との
  境界で UTF-8 ⇔ Shift-JIS(CP932) を変換する（ASCII は変換不要の高速パス）。
  SJIS の 2 バイト目に `.`(0x2E) が来ても拡張子区切りと誤認しない。

## ビルド

```sh
make            # → ./hu68disk
```

macOS は `libiconv`（`-liconv`、Makefile が自動付与）、Linux(glibc) は libc 内蔵で追加不要。

CMake からもビルドできる（全 OS 共通。リリース ZIP の `hu68disk` / `hu68disk.exe` はこれ）。

```sh
cmake -S . -B build && cmake --build build --target hu68disk
```

Windows（MSVC / clang-cl）は iconv を使わず、`src/peripheral/sjis.c` が Win32 の
`MultiByteToWideChar` / `WideCharToMultiByte` で CP932⇔UTF-8 を変換する。ホストのパスと引数は
UTF-8 として扱い（`compat.c` が `_wfopen` 等へ変換）、日本語のファイル名を指定できる。
OS 依存の処理は `compat.h` / `compat.c` に集約している。

## 使い方

```sh
hu68disk list    <image> [path]
hu68disk copy    <image> <hostfile> <destpath>
hu68disk del     <image> <path>
hu68disk extract <image> <path> <hostfile>
hu68disk mkdir   <image> <path>
hu68disk rmdir   <image> <path>
hu68disk partitions <image> [--all]
hu68disk copyall <src-image> <src-path> <dst-image> <dst-path> [--src-part N] [--dst-part N]
hu68disk mkfs    <image> [--type 2hd|scsi] [--size NN] [--label 名前] [--volume NN[:名前]]... [--removable] [--no-partition] [--force]
                                                                                  # 別名: format
hu68disk fsck    <image> [--fix] [--verbose] [--yes] [--no-backup]              # 別名: check
```

例:

```sh
hu68disk mkfs    new.xdf                       # 空の 2HD イメージを作成
hu68disk mkfs    new.hds --type scsi           # 空の SCSI HDD（既定10MB、XEiJ 互換）
hu68disk mkfs    new.hds --type scsi --size 20 --label MYDISK  # 20MB・ラベル付き
hu68disk mkfs    multi.hds --type scsi --size 100 --volume 40:SYSTEM --volume 60:DATA
hu68disk partitions multi.hds
hu68disk list    multi.hds --part 2 /
hu68disk mkfs    new.hds --type scsi --size 128 --label MO --removable  # MO相当（リムーバブル）
hu68disk mkfs    netbsd.hds --type scsi --size 2000 --no-partition      # 領域未確保の空HDS（他OS用）
hu68disk list    work.xdf                      # ルートを一覧
hu68disk list    work.xdf /SYS                 # サブディレクトリを一覧
hu68disk copy    work.xdf gvrampages.x /       # host の gvrampages.x をルートへ
hu68disk copy    work.xdf テスト.x /            # 日本語名も可（UTF-8→SJIS 変換）
hu68disk copy    work.xdf foo.x /BIN           # サブディレクトリへ
hu68disk copy    work.xdf foo.x /BIN/BAR.X     # リネームして格納
hu68disk extract work.xdf /SYS/ASK68K.SYS ./ask.sys
hu68disk mkdir   work.xdf /TOOLS
hu68disk rmdir   work.xdf /TOOLS               # 空のときのみ
hu68disk del     work.xdf /gvrampages.x
hu68disk fsck    work.xdf                      # 整合性チェック（診断のみ、書き換えなし）
hu68disk fsck    work.xdf --fix                # 検出した問題を修復（確認プロンプトあり）
hu68disk fsck    work.xdf --fix --yes          # 確認なしで修復（スクリプト向け）
```

- パス区切りは `/`。`copy` の `<destpath>` が既存ディレクトリなら host ファイルの
  basename を使う。`/DIR/NAME.EXT` を指定すればリネームして格納。同名があれば上書き。
- 名前の長さ制限は **バイト数**（名前部 18B＝全角 9 文字、拡張子 3B）。超過や
  不正文字（制御文字・`: ? *`）はエラー。

### SCSI HDD の複数パーティション

`partitions` は SCSI パーティションテーブルを表示する。`--part N` の `N` は「見つかった
順」ではなく、テーブルの **1〜15番スロット番号**である。空スロットも確認する場合は
`--all` を付ける。`list`、`copy`、`del`、`extract`、`mkdir`、`rmdir`、`fsck` はすべて
`--part N` で対象区画を選べ、指定なしは従来どおり先頭の使用可能な Human68k 区画を選ぶ。
FDイメージへの `--part` はエラーである。

`copyall` は入力・出力を独立して `--src-part N` / `--dst-part N` で選ぶ。同じHDS内の異なる
区画間コピーは可能だが、同一ファイル実体の同一区画へのコピーは拒否する。

`mkfs --type scsi --volume MiB[:LABEL]` は1〜15個の連続したHuman68k区画を生成する。
`--size` を併記した場合は各 `--volume` の合計と一致する必要がある。省略時は合計を使い、
`--size` と `--volume` の両方を省略した場合だけ従来どおり10MiBの単一区画となる。
`--volume` と `--label` は併用できない。第1区画はboot、第2区画以降はusableで作成する。

`mkfs --type scsi --no-partition` は Human68k の区画を1つも確保しない空の `.HDS` を作る。
XEiJ の「SCSI ハードディスクのイメージファイルの新規作成」で **「領域確保」を OFF** に
したときと同じ内容で、`--label` / `--volume` とは併用できない。用途と詳細は下記
「領域を確保しない空 HDS」を参照。

不明なオプション、値不足、重複オプション、不正な数値は終了コード2の使用法エラーとなる。
`--` 以降は位置引数として解釈する。この終了コード2の厳格化は、従来すべて1へ畳み込まれて
いた挙動からの互換性変更である。

### `mkfs` / `format` の挙動（ディスクの有無で分岐）

`mkfs` と別名 `format` は同じ動作。`--type 2hd`（既定）は空の 2HD/FAT12 イメージ
（1,261,568 バイト）、`--type scsi` は空の SCSI HDD/FAT16 イメージ（XEiJ 互換 `.HDS`）を作る。
`--size`（MB 単位、既定10）・`--label`（ボリュームラベル）・`--removable`・`--no-partition` は
`--type scsi` のみ有効。
`--size` はクラスタサイズを容量に応じて自動的に大きくする（XEiJ `HumanMedia.java` 同一
アルゴリズム、データ領域のクラスタ数を 65520 以下に保つ）ため、下限（FAT16 として
成立する約5MB）を超えていれば数百MB〜2GB級の指定も可能（実機の運用と同様。エミュレータの
SCSI実装がRead(10)による1GB越えアクセスに対応済みであることも実機起動込みで検証済み、
下記「検証済み」参照）。

`--removable` はデバイスパラメータ `$0F`(EJ) を `$01` にし、MO 等のリムーバブルメディアとして
扱う（Issue #38、`inside_x68000_1992.md` 図9）。エミュレータ側の Unit Attention 対応と組み合わせ、
稼働中に MCP `disk(action="insert"/"eject", media="scsi")` で reset 無しの
メディア交換ができる。

| ディスクの状態 | `--force` なし | `--force`（`-f`）あり |
|----------------|----------------|------------------------|
| **存在しない** | そのまま新規作成する（確認なし） | そのまま新規作成する |
| **既に存在する** | `フォーマットしますか？ (y/N):` と確認。`y`/`Y` で上書き、それ以外・EOF（パイプ等で入力なし）は **中止** | 確認なしで即上書き |

- 「存在しない」場合は失うデータが無いので確認しない。
- 「存在する」場合はディスクイメージの誤消去を防ぐため確認する。スクリプトから
  無人で上書きしたいときは `--force` を使う。

```sh
hu68disk mkfs   new.xdf            # 無ければ作成
hu68disk format work.xdf           # 有れば「フォーマットしますか？」と確認
hu68disk format work.xdf --force   # 確認なしで上書き
echo y | hu68disk format work.xdf  # スクリプトから確認に y を渡す
```

### 領域を確保しない空 HDS（`--no-partition`）

`--type scsi --no-partition` は、ディスク先頭 32KB のヘッダ
（SCSI デバイスパラメータ・Disk IPL・SCSI デバイスドライバ・`"X68K"` 署名のみの
パーティションテーブル）だけを書き、区画エントリ（`$0810`〜）も区画領域（`$8000`〜）も
すべて 0 のままにする。パーティションテーブル `$0804`（確保済みの最後の区画の末尾までの
セクタ数）には区画領域の開始位置 32 を書く＝「1 区画も確保していない」状態を表す。

XEiJ の `SCMedia.scmMakeFormatData()` を `partitioningOn=false` で呼んだ結果と同一で、
実際に XEiJ が作った `.HDS` と先頭 32KB をバイト比較して、`$0804` と第1区画エントリ
`$0810`〜`$081F` 以外は 1 バイトも違わないことを確認済み。

用途は **NetBSD/x68k のように自前でディスクラベルとファイルシステムを書く OS の
インストール先**。通常の `mkfs --type scsi` が作る Human68k/FAT16 区画はディスク全体を
占有するため、NetBSD の `sysinst` からは `f: ... = (Other: MSDOS)` として見え、
root ファイルシステムを確保できない。

区画本体は書き込まずにファイル末尾だけを伸ばして確保するので、穴あきファイルに対応した
ファイルシステムでは 2GB のイメージでも実消費は数十 KB で済む。

```sh
hu68disk mkfs netbsd.hds --type scsi --size 2000 --no-partition
# mkfs: netbsd.hds (SCSI HDD, unpartitioned, 2048032 sectors)
```

作成直後は Human68k 区画が無いため、`list` / `copy` / `fsck` 等の区画を要する操作は
`error: 指定パーティションがありません` となる（`partitions --all` で全スロットが空
であることは確認できる）。

### 大容量イメージのメモリ使用量

イメージ全体ではなく **必要な範囲だけ**をメモリへ読む。

| コマンド | 読む範囲 |
|----------|----------|
| `partitions` | 先頭 2,304 バイト（デバイスパラメータ＋パーティションテーブル）＋ラベル表示のため区画ごとにルートディレクトリ領域まで |
| `list` / `copy` / `del` / `extract` / `mkdir` / `rmdir` / `fsck` / `copyall` | 選択した区画（`--part N`）だけ |
| FD イメージ（`.xdf` / `.hdm` / `.2hd`） | 全体（最大 1.44MB） |

書き戻しも選択区画の範囲だけを書くので、他の区画は書き込み対象にならない。
`fsck --fix` の `.bak` はファイル→ファイルのチャンクコピーで作るため、
イメージサイズに比例した常駐は発生しない。

2GB `.HDS`（100MiB + 1900MiB の2区画）での実測（macOS の maximum resident set size）:

| コマンド | 従来 | 現在 |
|----------|-----:|-----:|
| `partitions` | 2001.5 MiB | 1.7 MiB |
| `list --part 1` | 2001.5 MiB | 101.4 MiB |
| `fsck --part 1 --fix`（`.bak` 作成込み） | 2001.5 MiB | 101.7 MiB |

区画そのものが GB 級なら、その区画分は確保する（上記の `--part 2` は 1901.4 MiB）。

### `copyall`（システム転送・COPYALL /S 相当）

`<src-image>` の `<src-path>` 配下を `<dst-image>` の `<dst-path>` へ**再帰的に**コピーする。
実機 Human68k の `COPYALL /S A:\*.* C:` に相当し、フロッピーのシステムディスク一式を
新規作成した SCSI HDD (`.HDS`) へ丸ごと転送するために使う。

```sh
hu68disk mkfs    boot.hds --type scsi --size 40 --label SYSTEM_HD
hu68disk copyall HUMAN302.XDF / boot.hds /   # システムディスク一式
hu68disk copyall X68K_DIC.XDF  / boot.hds /   # 辞書ディスクも同じ場所へ（2回目）
```

- `<dst-path>` が存在しなければ `mkdir -p` 相当で作成する。既存のサブディレクトリは
  作成し直さず流用するため、**上の例のように2枚以上のディスクを同じ宛先へ順番に
  copyall しても安全**（実機で複数枚のディスクに対し `COPYALL` を繰り返す運用と同じ）。
- ファイル属性（system/hidden/readonly）・タイムスタンプはコピー元のまま引き継ぐ。
- ボリュームラベル・削除済みエントリ・VFAT LFN 断片はコピー対象外（`list` と同じ規則）。
- 同じHDSの異なる区画間も `--src-part` / `--dst-part` でコピーできる。ファイル実体と
  選択区画の両方が同じコピーだけは拒否する（誤った自己コピー防止）。

### `fsck` / `check`（整合性検査・修復）

FAT12（2HD/XDF）・FAT16（SCSI HDD/.HDS）両対応。既定は **診断のみ**（イメージを一切
書き換えない）。`--fix` で検出した問題を修復する。

検査項目: FAT予約マーカー、VFATロングファイル名混入（Issue #30 対策）、
`.`/`..`の指すクラスタ、クラスタチェーンの範囲外参照・循環・クロスリンク、
ファイルサイズとチェーン長の不整合、lost cluster（どのファイルからも参照されない
割当済みクラスタ）。

第1FATと第2FAT以降の一致検査は行わない: `XC20_ProgManual_appendix.md` 付録2に
「第2FAT（未使用）」と明記されており、Human68k はそもそも第2FATを維持しない。実際
XEiJ生成イメージを含むほぼ全ての実イメージで食い違っており、検出すると誤検知だらけに
なるため（2026-07-13）。

| フラグ | 意味 |
|--------|------|
| （なし） | 診断のみ。1バイトも書き換えない。 |
| `--fix` | 検出した問題を修復してイメージに書き戻す。 |
| `--verbose` / `-v` | 各パスの詳細（走査したファイル数など）も表示。 |
| `--yes` / `-y` | `--fix` 時の確認プロンプトを省略（スクリプト向け）。 |
| `--no-backup` | `--fix` 時に自動作成する `<image>.bak` を作らない。 |

`--fix` は既定で `<image>.bak` へ修復前の状態をバックアップしてから書き換える。
修復後は自動的に再検査し、直せなかった問題（不正なファイル名文字、ディレクトリの
クラスタが完全に失われている場合など）が残っていれば警告して終了コード 1 を返す。

終了コード: `0`=クリーン（または全問題を修復）、`1`=問題が残っている、`2`=使用法エラー。

現行設計は `design_hu68disk.md` を参照。

## 検証済み（実機 Human68k v3.02, 本リポジトリのエミュレータで `DIR` 確認）

- **Phase 1**: 長名 `gvrampages.x`・最大長 `ABCDEFGHIJKLMNOP12.X`・日本語 `テスト.X`
  を白帯（無効メディア）なしで正しく表示。VFAT LFN（属性 0x0F）を含まない単一 32B
  エントリ。`del` の反映、host mtime の FAT 日時反映。
- **Phase 2**: `mkfs` で作った空ディスクが `DIR B:` で読める（ラベル無しは
  「ボリュームがありません」の正常表示）。`mkdir` で作った `/TOOLS`・深いネスト
  `/TOOLS/SUB/deep.x` を `DIR B:\TOOLS`／`DIR B:\TOOLS\SUB` で確認。`extract` は
  5000 バイトのランダムデータで往復バイト一致。`rmdir` は空／非空を判別。

## 検証済み（Phase 3, SCSI HDD / XEiJ 互換, 2026-07-09）

XEiJ で作成した実 SCSI HDD イメージ（10MB, `.HDS`）を使い、`list`/`copy`/`extract`/`del`/
`mkdir`/`rmdir`（サブディレクトリ含む）を実データで検証済み。FAT16 の実装はこの Phase で
初めて end-to-end 検証された。`mkfs --type scsi` は生成したイメージが XEiJ 実イメージと
**メタ領域（デバイスパラメータ・IPL・パーティションテーブル・デバイスドライバ・BPB、
`0x0000`〜`0x8400`）が1バイトも違わず完全一致**することを確認済み。

**IPL/デバイスドライバの埋め込みは必須**（当初「データ専用ディスクなら不要」と想定していたが
実機検証で誤りと判明）: `SPC_DISK_IPL`(0x0400)・`SPC_DEVICE_DRIVER`(0x0C00)・
`SPC_PARTITION_IPL`(0x8000〜) が無いと、BPB・パーティションテーブルが完全に正しくても
Human68k はドライブとして認識しない。これらは XEiJ (`SPC.java`) から抽出したバイト列を
`tools-host/hu68disk/scsi_ipl_data.h` に格納し常時書き込む（出典・ライセンス根拠は同ファイル参照）。

**HD の FAT16 エントリ（クラスタ連結）はビッグエンディアン**（ディレクトリエントリの
CLUSTER/SIZE/TIME/DATE は FD と同じリトルエンディアンのまま）。実データで確認済み。

**実 Human68k とのクロス検証**: `tools-x68k/asmtest/scsi_write_test.s`（複数セクタにまたがる
既知パターンをファイル書き込みするテストプログラム）を XEiJ 上の実 Human68k で実行し、
`hu68disk extract` で取り出した内容が期待パターンと完全一致することを確認。書き込み側
（実 Human68k FAT16 ドライバ）と読み出し側（`hu68disk`）が独立実装のまま一致した、最も
強い検証。

現行設計は `design_hu68disk.md` を参照。

## 検証済み（`mkfs --type scsi`の可変クラスタサイズ対応、2026-07-28）

`--size`を10/20/40/64MB（従来どおりクラスタ=1セクタで足りる範囲）に加え、
100MB/500MB/1024MB(1GB)/2048MB(2GB)/4096MB(4GB)で作成し、いずれも`fsck`で問題なし
（1GBは1クラスタ=16KB・65518クラスタを自動選択）を確認。1GBイメージへの
`copy`/`mkdir`/`extract`（バイト完全一致）・`copyall`も正常動作を確認。
5MB未満（FAT16として成立するクラスタ数4085未満）はエラーで正しく弾かれることも確認。
これにより実機で運用されていた1GB級のHDDと同等のイメージが作成できるようになった
（従来は1クラスタ=1セクタ固定の簡略実装により64MBが実質上限だった。2GBまでの実機起動込みの
検証は次項参照）。

## 検証済み（2GB `.HDS` からのSCSI起動・1GB境界超えアクセス、2026-07-28）

「Human68kは最大2GBだったはず」という指摘を受け、実際にエミュレータで2GBイメージが
動作するか実機起動込みで検証した。

- 実物の`HUMAN302.XDF`（Human68k v3.02システムディスク）から`copyall`で2GBの`.HDS`を作成
- 意図的に**生バイトオフセット1,159,494,656（約1.08GB地点、1GiB境界超え）**へ
  マーカーファイルを配置（`grep -a -b -o`で実際の配置オフセットを確認）
- ビルドしたエミュレータでこの2GBイメージからSCSI起動 → `A>`プロンプト到達を確認
- `DIR`で境界超えのファイルが正しく一覧表示、`TYPE`でバイト完全一致の内容読み出しを確認

SCSIの`Read(6)`コマンドはLBAが21bit固定（`src/peripheral/scsi.c`）のため512Bブロック換算
1GiBまでしかアドレスできないが、実機Human68kのSCSIドライバ（ディスク側に転送される
実バイナリ）は1GBを超えると自動的に32bit LBAの`Read(10)`へ切り替える。エミュレータの
`Read(10)`実装（同ファイル）がこれに正しく応答することを、この実機起動テストで実証した。

## 検証済み（`copyall`、2026-07-28）

ルート直下2ファイル+サブディレクトリ1件（ファイル1件）を持つソースイメージから
新規SCSI HDDイメージへ`copyall`し、宛先の`list`表示・`extract`によるバイト完全一致・
`fsck`0件を確認。さらに**2枚目の別ソースイメージ**（辞書ディスク相当、同じ`/SYS`に
別ファイルを持つ）を同じ宛先へ`copyall`し、既存`/SYS`サブディレクトリが再作成されず
両ソース由来のファイルが共存すること（実機で複数ディスクにCOPYALLを繰り返す運用と
同等）、`fsck`が問題なしを返すことを確認。

## 対応状況・今後

| 項目 | 状態 |
|------|:----:|
| FAT12 (XDF/2HD) | ✅ Phase 1 |
| ルート list/copy/del | ✅ Phase 1 |
| Human68k ネイティブ長名 (18+3) | ✅ Phase 1 |
| Shift-JIS 名（日本語） | ✅ Phase 1 |
| サブディレクトリのパス（`/BIN/foo` 等） | ✅ Phase 2 |
| extract / mkdir / rmdir | ✅ Phase 2 |
| mkfs（空 2HD イメージ作成） | ✅ Phase 2 |
| SCSI HDD (.HDS) イメージ（XEiJ 互換） | ✅ Phase 3 |
| FAT16（HD, BE エンディアン） | ✅ Phase 3 |
| mkfs --type scsi（--size・--label） | ✅ Phase 3 |
| mkfs --type scsi --removable（MO相当、reset無しホットスワップ） | ✅ Issue #38 |
| mkfs --type scsi --no-partition（領域未確保の空HDS、他OS用） | ✅ |
| fsck / check（整合性検査・修復） | ✅ |
| copyall（COPYALL /S 相当、イメージ間再帰コピー） | ✅ Issue #14 |
| 大容量イメージの部分マップ（GB級 `.HDS` を全読みしない） | ✅ Issue #151 |
| 実機（本リポジトリのエミュレータ）でのマウント確認 | ✅ Issue #22 |
| FAT16 / HDD の複数パーティション・SASI HDD | Phase 3 以降・非スコープ |
| Windows 対応（MSVC / clang-cl、日本語パス・引数） | ✅ Issue #31 |
| Linux 対応 | リリース ZIP 組み立て時に確認（Issue #31） |

今後の詳細は Issue #31 で管理。

一次資料: 『C Compiler PRO-68K ver2.0 プログラマーズマニュアル』（XC20）付録2
（ディスクマップ/FAT構造）・③ディレクトリエントリ構造、実ディスク HUMAN302.XDF の実測、
および XEiJ ソース（`SCMedia.java`/`HumanMedia.java`/`SPC.java`）・XEiJ で生成した実
SCSI HDD イメージの実測（`design_hu68disk.md`）。
