# AIエージェントによるHDDインストール手順（自然文1つで完結させる）

Claude Code や LM Studio 等、**ホストのシェルコマンドを実行できるAIエージェント**に対し、
次のような自然文を渡すだけで、起動可能なSCSI HDD (`.HDS`) イメージを作成できるようにする
ための固定手順（レシピ）。

> 200Mバイトの空きハードディスクを用意して、フォーマットして、Human68kの起動ディスクに
> して、ボリューム名はSYSTEM_HD

Human68k ユーザーズマニュアル2.9節「システム、辞書ディスクの内容を内蔵ハードディスク
に転送する」の`COPYALL /S A:\*.* C:`相当を、ホスト側で`hu68disk copyall`により再現する
（フロッピー起動よりシステム起動・日本語入力が高速になるという実機と同じ利点）。

**この手順はAIエージェントが読んで実行することを想定している**（都度LLMが手順を考案する
のではなく、毎回同じ手順を辿らせて再現性を確保するのが目的）。人間が手動で行う場合も
そのまま使えます。

---

## 前提

| 項目 | 確認方法 |
|------|---------|
| `hu68disk` が使える | リリースZIPでは `tools-host/hu68disk/` にビルド済みの `hu68disk`（Windowsは `hu68disk.exe`）が入っている。ソースから使う場合は `tools-host/hu68disk/` で `make`（`./hu68disk` が生成される） |
| `sram_switch` が使える（メモリ容量を変える場合のみ必須） | リリースZIPでは `tools-host/sram_switch/` にビルド済みの `sram_switch`（Windowsは `sram_switch.exe`）が入っている。ソースから使う場合は `tools-host/sram_switch/` で `make` |
| Human68kシステムディスクイメージを入手済み | 例: `disk/HUMAN302.XDF`。著作権上リポジトリに同梱していないため、[QUICKSTART](../../../../QUICKSTART.md)の「ROM・ディスクイメージを用意する」または[emulator_install.md](emulator_install.md)のStep 4-3に従いユーザー自身が用意する |
| （任意）ASK68K辞書ファイル（`X68K.DIC`） | 日本語入力を使う場合。無ければStep 3-2は省略。入手先例: `http://retropc.net/x68000/software/sharp/sxwindow/SXWIN315.LZH`（SX-WINDOW ver3.1 辞書ディスク、`POST.DIC`/`X68K.DIC`収録。無償公開されたシャープのソフトウェア、動作検証済み・2026-08-14） |

**コマンドのパスについて**: 本手順のコマンド例は、リポジトリまたはリリースZIPの展開先をカレント
ディレクトリとし、`tools-host/hu68disk/hu68disk`・`tools-host/sram_switch/sram_switch`・
`disk/<ファイル名>` の相対パスで書いている。[emulator_install.md](emulator_install.md)で導入した
ディレクトリ（`hu68disk`・`sram_switch` を直下へコピーした構成）で実行する場合は、
`<Q1のディレクトリ>` に`cd`した上で `./hu68disk`・`./sram_switch` に読み替える
（`tools-x68k/hostdrv/HOSTDRV.SYS` など他の相対パスも、導入先に無ければリポジトリ側のパスを指定する）。

---

## Step 1: ユーザーに4項目を順番に質問する

**1問ずつ**、以下の文言で聞く（複数まとめて聞かない）。

### Q1. HDD容量（MB）

> 作成するハードディスクの容量をMB単位で教えてください（推奨: 200MB。実機でも200MB級の
> HDDが販売されていました。2GB級までの指定も動作確認済みです。クラスタサイズが容量に
> 応じて自動的に大きくなるため、FAT16の仕様上の下限（5MB程度）を下回らなければ基本的に
> 上限はありません）。

- 未回答・「おまかせ」等なら **200** を既定値として採用します。
- 5未満の整数が来たら「FAT16の最小クラスタ数に満たない」旨を伝えて聞き直す
  （`hu68disk mkfs`自体もエラーで弾くが、事前に案内した方が手戻りが無い）。

### Q2. ボリューム名

**先に制約を提示してから**聞く（後出しで文字数超過が発覚すると、`hu68disk`が警告なく
11バイトへ切り詰めてしまうため）。

> ボリューム名を教えてください。制約: 半角なら11文字、全角なら5文字まで（混在も合計
> 11バイト以内）。使える文字は `A-Z 0-9 & # ( ) @ - ^ { } !` とカナ・全角文字です
> （小文字は入力できますが大文字として登録されます）。

- 提示した制約を超える・使用不可文字を含む回答が来たら、**`hu68disk`を呼ぶ前に**
  再確認する（`hu68disk mkfs --label`は超過分を黙って切り詰めるだけでエラーにしない
  ため、ツール任せにしない）。

### Q3. `.HDS`ファイル名

> 作成するディスクイメージのファイル名を教えてください（例: `SYSTEM_HD.HDS`。保存先は
> `disk/` ディレクトリを想定しています）。

- 拡張子`.hds`/`.HDS`が無ければ補います。
- 既存ファイルと衝突する場合は上書きしてよいか確認する（`hu68disk mkfs`はデフォルトで
  既存ファイルへの上書き確認プロンプトを出すが、非対話実行になるため事前確認が必要）。

### Q4（おまけ）. メモリ容量

> ついでにメモリ容量（KB単位）も変更しますか？（推奨: 12288＝12MB。実機の実用的な
> 上限に近く、大半のソフトが快適に動きます。変更しない場合は「いいえ」とお答え
> ください。sram.datの現在値は `sram_switch` の引数無し実行で確認できます）

- 未回答・「おまかせ」等なら**12288**を既定値として採用します。
- 「いいえ」「skip」等が明示されたときのみStep 4を省略し、`sram.dat`は一切変更しません。
- 数値が来たら次のStepでそのまま使う（`sram_switch`自体が範囲外値をエラーにする）。

### Q5（おまけ）. AUTOEXEC.BATのFASTIO高速化ブロックを削除するか

> 転送される`AUTOEXEC.BAT`には、実機の物理FD/HDDを高速化するTSR（`FASTIO`/`FASTSEEK`/
> `FASTOPEN`）を組み込むブロックが含まれています。エミュレータ環境ではディスクI/Oは
> 既にホスト側でエミュレートされているため、これらを組み込んでも実機のような高速化効果は
> ありません。削除することを推奨します。削除しますか？（推奨: 削除する）

- 未回答・「おまかせ」等なら**削除する**を既定値として採用し、Step 3-3を実施します。
- 「いいえ」「残す」等が明示されたときのみ**Step 3-3は省略**。

### Q6（おまけ）. プロンプトをパス付きにするか

> プロンプトを`A>`のようなドライブ文字だけの表示から、`A:\BIN>`のようにカレント
> ディレクトリまで表示する形式に変更しますか？（推奨: 変更します。カレントディレクトリが
> 一目でわかり操作しやすくなります）

- 未回答・「おまかせ」等なら**変更する**を既定値として採用し、Step 3-4を実施します。
- 「いいえ」「変更しない」等が明示されたときのみ**Step 3-4は省略**。

### Q7（おまけ）. MOドライブを接続するか

> ついでにMOドライブも接続しますか？（汎用SCSIドライバ**SUSIE.X**経由で、空の
> リムーバブルドライブとして接続されます。メディア未挿入の状態から始まります。
> この経路は現時点で動作未検証です）

- 未回答・「おまかせ」等なら**接続しない**を既定値として採用する（HDD作成が主目的の
  ため、明示の要望がある場合のみ追加する）。
- 「はい」が明示されたときのみStep 3-5でSUSIE.Xを導入し、Step 5-2でSCSI ID設定を追記します。

### Q8（おまけ）. CD-ROMドライブを接続するか

> ついでにCD-ROMドライブも接続しますか？（汎用SCSIドライバ**SUSIE.X**経由で、空の
> CD-ROMドライブとして接続されます。メディア未挿入の状態から始まります。動作検証済み
> の経路です）

- 未回答・「おまかせ」等なら**接続しない**を既定値として採用します。
- 「はい」が明示されたときのみStep 3-5でSUSIE.Xを導入し、Step 5-2でSCSI ID設定を追記します。

### Q9（おまけ）. HOSTDRV.SYSを組み込むか

> ついでにHOSTDRV.SYSも組み込みますか？ホストOS（macOS/Windows/Ubuntu）側の指定
> ディレクトリを、固定ジオメトリの読み取り専用FAT16ドライブとしてHuman68kから
> 参照できるようになります（ファイル転送用）。

- 未回答・「おまかせ」等なら**組み込まない**を既定値として採用します。
- 「はい」が明示されたときのみ、続けて共有するホストOS側ディレクトリの絶対パスを聞く
  （**Q9-2**。個人環境のパスをそのまま流用しない——`ix68k.cfg`の`hostdrv_dir`は
  ユーザーごとに異なるため、必ず本人に確認する）。回答を得たらStep 3-6でドライバを
  導入し、Step 5-3で`ix68k.cfg`の`hostdrv_dir`を設定します。

---

## Step 2: `.HDS`イメージを作成する

```sh
tools-host/hu68disk/hu68disk mkfs disk/<Q3のファイル名> --type scsi --size <Q1> --label <Q2>
```

## Step 3: システム（+辞書）を転送する

```sh
# 1. システムディスク（必須）
tools-host/hu68disk/hu68disk copyall <システムディスクイメージ> / disk/<Q3のファイル名> /

# 2. 辞書ディスク等（あれば、複数回実行してよい。既存ディレクトリへ安全にマージされる）
tools-host/hu68disk/hu68disk copyall <辞書ディスクイメージ> / disk/<Q3のファイル名> /
```

実行後、`hu68disk fsck disk/<Q3のファイル名>` で0件（問題なし）であることを確認します。

### Step 3-2（辞書ファイルがXDFではなく単体配布の場合）: X68K.DICを取得・配置してCONFIG.SYSを直す

`前提`の入手先例（`SXWIN315.LZH`等）はディスクイメージではなく個別ファイル配布のため、
`copyall`ではなく`copy`で配置し、`CONFIG.SYS`のASK68K辞書パスを手動で直す
（動作検証済み、2026-08-14）。**`X68K.DIC`はルート直下ではなく`\ASK`（ASK68Kの環境ファイル
`ENV*.ASK`と同じ既存ディレクトリ）に置く**（ルート直下にファイルを増やしたくない場合の運用。
ルートに置いても動作上は問題ない）。

```sh
curl -L -o /tmp/SXWIN315.LZH "http://retropc.net/x68000/software/sharp/sxwindow/SXWIN315.LZH"
mkdir -p /tmp/sxwin315_extracted && cd /tmp/sxwin315_extracted && unar -f -D /tmp/SXWIN315.LZH
tools-host/hu68disk/hu68disk copy disk/<Q3のファイル名> /tmp/sxwin315_extracted/X68K.DIC /ASK
```

`CONFIG.SYS`の`DEVICE = \SYS\ASK68K.SYS /D<d>:<path>\X68K.DIC ...`の`<d>`は、辞書ファイルを
置いたドライブ（＝このHDD自身）の**現時点のドライブレター**、`<path>`は配置先ディレクトリ
（`\ASK`）でなければなりません。デフォルトの`HUMAN302.XDF`由来のCONFIG.SYSは
`/DB:\X68K.DIC`（実機で辞書をFD1=B:のルートに挿す運用が前提）になっているため、そのままでは
辞書ドライバが見つかりません。

```sh
tools-host/hu68disk/hu68disk extract disk/<Q3のファイル名> /CONFIG.SYS /tmp/CONFIG.SYS.orig
# /DB:\X68K.DIC を /D<HDDの現ドライブレター>:\ASK\X68K.DIC に置換
perl -pi -e 's/\/DB:\\X68K\.DIC/\/D<HDDの現ドライブレター>:\\ASK\\X68K.DIC/' /tmp/CONFIG.SYS.orig
tools-host/hu68disk/hu68disk copy disk/<Q3のファイル名> /tmp/CONFIG.SYS.orig /CONFIG.SYS
tools-host/hu68disk/hu68disk fsck disk/<Q3のファイル名>
```

**Step 6-3（ドライブ入れ替え）を実施する場合は、入れ替え後の最終ドライブレターに合わせて
この置換をやり直すこと**（先にStep 3-2をやってからStep 6-3をやる場合、Step 6-3の中で
再度`CONFIG.SYS`を直します。動作検証した例ではHDDが最終的にC:になったため`/DC:\ASK\X68K.DIC`）。

**重要: AUTOEXEC.BATを編集するときは改行コードを必ずCRLFにすること**（Step 3-3・3-4・
6-3共通）。X68000/Human68kのテキストファイルはMS-DOS同様CRLFが標準（元の`AUTOEXEC.BAT`を
`od -c`等で確認すると`\r\n`になっている）。Writeツール等でLFのまま保存するとix68k上での
見た目が崩れる・一部ツールで正しく解釈されない可能性があります。動作検証（2026-08-14、
別ディレクトリへの新規インストールでのテスト実行）でLF書き込みのまま気づかず進めてしまった実例あり。

### Step 3-3（Q5で「削除する」の場合のみ）: AUTOEXEC.BATからFASTIOブロックを削除する

```sh
tools-host/hu68disk/hu68disk extract disk/<Q3のファイル名> /AUTOEXEC.BAT /tmp/AUTOEXEC.BAT.orig
```

以下の6行（`SET FASTIO=DONE`から`:SKIP`まで）を取り除いてから書き戻す
（実際のHuman68kのFD/HDD高速化用ブロック。IDE等のホストストレージを介するエミュレータ環境では
組み込む意味が無いため）:

```
SET FASTIO=DONE
IF %FASTIO% == DONE GOTO SKIP
FASTIO 384 -S32 -D -F -E
FASTSEEK 128
FASTOPEN 100
SET FASTIO=DONE
:SKIP
```

```sh
# <編集後のAUTOEXEC.BAT>はCRLF改行で保存すること（上記「重要」参照）
tools-host/hu68disk/hu68disk copy disk/<Q3のファイル名> <編集後のAUTOEXEC.BAT> /AUTOEXEC.BAT
tools-host/hu68disk/hu68disk fsck disk/<Q3のファイル名>
```

Step 6-3で`AUTOEXEC.BAT`をさらに編集する場合は、その時点の最新内容を`extract`し直してから
編集すること（Step 3-3の変更を上書きしないよう、都度最新状態から編集する）。

### Step 3-4（Q6で「はい」の場合のみ）: プロンプトをパス付きにする

```sh
tools-host/hu68disk/hu68disk extract disk/<Q3のファイル名> /AUTOEXEC.BAT /tmp/AUTOEXEC.BAT.orig
```

末尾に以下の1行を追記してから書き戻す（`PROMPT`は内部コマンドのため`PATH`未設定でも動作し、
どの位置に追記しても問題ないが、慣例として末尾に置きます。動作検証済み: `A>`が`A:\>`、
`CD BIN`後は`A:\BIN>`のようにカレントディレクトリ追従を確認）:

```
PROMPT $P$G
```

```sh
# <編集後のAUTOEXEC.BAT>はCRLF改行で保存すること（Step 3-3冒頭「重要」参照）
tools-host/hu68disk/hu68disk copy disk/<Q3のファイル名> <編集後のAUTOEXEC.BAT> /AUTOEXEC.BAT
tools-host/hu68disk/hu68disk fsck disk/<Q3のファイル名>
```

Step 3-3・Step 6-3と同様、既に他の編集を行っている場合はその時点の最新内容を`extract`し
直してから追記すること。

### Step 3-5（Q7またはQ8で「はい」の場合のみ）: SUSIE.Xを配置し、CONFIG.SYSへ登録する

MO・CD-ROMのどちらも、Human68k標準のSCSIドライバではなく汎用SCSIドライバ**SUSIE.X**
（GORRY作、"System of Universal SCSI Integrated Engine"）を使います。メディア未挿入のドライブ
にもドライブ名を確保でき、DOSコール経由のメディア交換をリセット無しで認識する
（詳細: `docs/x68k-usage/freeware/SUSIE.X.md`）。

導入前にユーザーへ次のように説明する:

> SUSIE.X（GORRY作の汎用SCSIドライバ）を
> http://retropc.net/x68000/software/disk/scsi/susie/ からダウンロードして
> インストールします。

```sh
# 1. ダウンロード（arc/に既にあればスキップ。配布元サーバへの再アクセスを避ける）
curl -L --create-dirs \
  -o "arc/retropc.net/x68000/software/disk/scsi/susie/sse_121a.lzh" \
  "http://retropc.net/x68000/software/disk/scsi/susie/sse_121a.lzh"

# 2. 展開
unar -e SHIFT-JIS -o ./susie_extracted \
  "arc/retropc.net/x68000/software/disk/scsi/susie/sse_121a.lzh"
# → ./susie_extracted/ 以下に SUSIE.X ほか（susie.doc等）

# 3. 本体を \SYS へ
tools-host/hu68disk/hu68disk copy disk/<Q3のファイル名> ./susie_extracted/<展開後パス>/SUSIE.X /SYS
```

`CONFIG.SYS`へ、接続するデバイスごとに1行ずつ`DEVICE=`行を追記する（**1行＝1 SCSI ID**。
複数デバイスを1行にまとめることはできない）。挿入位置は既存の`DEVICE=`行のブロック内でよい。

```
DEVICE = \SYS\SUSIE.X -ID1 *:    ; Q7で「はい」の場合（MO）
DEVICE = \SYS\SUSIE.X -ID2 *:    ; Q8で「はい」の場合（CD-ROM）
```

**`-IDn`の数値は、Step 5-2で`ix68k.cfg`に設定する`scsi_diskN`のNと必ず一致させること**
（同じSCSI IDを指しているため。ユーザーに確認を求める必要はなく、この手順を実行する側が
矛盾なく揃える）。

```sh
tools-host/hu68disk/hu68disk extract disk/<Q3のファイル名> /CONFIG.SYS /tmp/CONFIG.SYS.orig
# 末尾のDEVICE=行の後ろに該当する1〜2行を追記（CRLF維持。Step 3-2「重要」参照）
tools-host/hu68disk/hu68disk copy disk/<Q3のファイル名> /tmp/CONFIG.SYS.orig /CONFIG.SYS
tools-host/hu68disk/hu68disk fsck disk/<Q3のファイル名>
```

**検証状態**: CD-ROM(`-ID2`)経路は動作検証済み✅。MO(`-ID1`)経路は
未検証📖（`SUSIE.X.md`の検証メモ参照。CD-ROM経路のみを検証したため変更していません。
Step 6の起動確認で問題が出ないか特に注意すること）。

### Step 3-6（Q9で「はい」の場合のみ）: HOSTDRV.SYSを配置し、CONFIG.SYSへ登録する

```sh
# 本体を \SYS へ（HOSTDRV.SYSはビルド済みのものがリポジトリ・リリースZIPに入っている。
# 再ビルドが必要な場合のみ make -C tools-x68k/hostdrv）
tools-host/hu68disk/hu68disk copy disk/<Q3のファイル名> tools-x68k/hostdrv/HOSTDRV.SYS /SYS
```

`CONFIG.SYS`へ`DEVICE=`行を1行追記する（既存の`DEVICE=`行のブロック内でよい）。

```
DEVICE = \SYS\HOSTDRV.SYS
```

```sh
tools-host/hu68disk/hu68disk extract disk/<Q3のファイル名> /CONFIG.SYS /tmp/CONFIG.SYS.orig
# 末尾のDEVICE=行の後ろに上記1行を追記（CRLF維持。Step 3-2「重要」参照）
tools-host/hu68disk/hu68disk copy disk/<Q3のファイル名> /tmp/CONFIG.SYS.orig /CONFIG.SYS
tools-host/hu68disk/hu68disk fsck disk/<Q3のファイル名>
```

## Step 4（Q4で数値が来た場合のみ）: メモリ容量を設定する

```sh
tools-host/sram_switch/sram_switch M=<Q4のKB値>
```

デフォルトでは`sram.dat`を直接書き換える（`.bak`へ自動バックアップ）。**エミュレータを
起動したままだと終了時に上書きされ反映されないため、必ずエミュレータ停止中に実行する**
（`sram_switch`はオフライン専用ツール、`/mcp-debug`参照）。

## Step 5: `ix68k.cfg`にHDDを接続する

`scsi_disk0`（未使用のSCSI IDがあればそちらでも可）に作成したイメージのパスを設定する
よう案内します。既存の`scsi_disk0`が別用途で使われている場合は上書きせず、空いている
`scsi_disk1`〜`scsi_disk6`を使う（`ix68k.cfg`の既存コメントアウト行を参照）。

```
scsi_disk0 = disk/<Q3のファイル名>
```

### Step 5-2（Q7またはQ8で「はい」の場合のみ）: MO/CD-ROMドライブを接続する

HDDが使ったSCSI IDとは別の空いている`scsi_disk1`〜`scsi_disk6`に設定する（値は大小文字
区別なし。`ix68k.cfg`の既存コメントアウト行を参照）。**Step 3-5でCONFIG.SYSに書いた
`-IDn`と同じ番号のスロットに合わせること。**

```
scsi_disk1 = MODRV   ; Q7で「はい」の場合（MO）
scsi_disk2 = CD       ; Q8で「はい」の場合（CD-ROM）
```

### Step 5-3（Q9で「はい」の場合のみ）: `ix68k.cfg`の`hostdrv_dir`を設定する

Q9-2で聞いたホストOS側ディレクトリの絶対パスを設定します。

```
hostdrv_dir = <Q9-2の絶対パス>
```

## Step 6: 起動デバイスをこのHDDに切り替え、フロッピーのドライブレターを維持する

**HDDへのシステムインストールが完了したら、6-1（起動装置をSCSI0に設定）は標準で実施する**
（HDDを作った目的はそこから起動することなので、明示の要望が無くてもデフォルトで行います。
`scsi_disk0`に接続しただけでもSRAMの起動装置設定が`STD`＝規定順(FD0→FD1→SCSI0→SCSI1)なら
フロッピー未挿入時は自動的にこのHDDから起動するが、フロッピーを挿したままでもHDD優先で
確実に起動させるため、明示的にSCSI0を設定する）。6-2（フロッピーのドライブレター維持）は
引き続きユーザーに確認します。

### 6-1. 起動装置をSCSI0に設定する

```
SWITCH B=SCSI0
```

（エミュレータ上でHuman68k起動後にこのコマンドを実行。動作検証済み: SRAM `$ED0018`を
書き換え、リセット後にFDDを無視してSCSI0から起動します。`docs/x68k-usage/official/SWITCH.X.md`
参照）。

### 6-2. ユーザーに質問する: フロッピーのドライブレターを維持するか

**動作検証で判明した重要な注意点**: HDD起動時はそのHDD自身がドライブ**A:**になり、
フロッピーは後ろのレター（C:/D:等）に押し出されます。この上で、以下の文言でユーザーに聞きます。

> HDDから起動すると、そのHDDがA:になりフロッピーはC:/D:等に押し出されます。フロッピーを
> 従来通りA:/B:のままにしたいですか？（「いいえ」ならHDDがA:のままで問題ありません。
> 追加の設定は不要です）

- 「いいえ」「HDDがA:でよい」等なら、**6-3は省略**し、Step 7（結果報告）へ進みます。
  ドライブ入れ替えは不要です。
- 「はい」「フロッピーをA:/B:のままにしたい」等なら、6-3へ進みます。

### 6-3.（6-2で「はい」の場合のみ）AUTOEXEC.BATを編集し、フロッピーのドライブレターをA:/B:に保つ

`AUTOEXEC.BAT`の先頭（`ECHO OFF`の直後）に`DRIVE.X`によるドライブ名交換を追記し、末尾の
`PATH`行を交換後の実際のドライブレターに書き換える（`docs/x68k-usage/official/DRIVE.X.md`参照）。
**HDDがA:、フロッピー0がB:、フロッピー1がC:の場合**（`DRIVE`引数なしで事前に確認すること、
決め打ちしない）の動作検証済み例:

```
ECHO OFF
A:\BIN\DRIVE A: B: > NUL
B:\BIN\DRIVE B: C:
PATH C:\;C:\SYS;C:\BIN;C:\BASIC2;C:\ETC;
SET FASTIO=DONE
...（以下は元のAUTOEXEC.BATのまま）
```

- 1回目のスワップは`> NUL`で出力を捨てます。2回目（最後）だけ画面に結果を表示させ、
  起動時に「ドライブ入れ替えが行われたこと」を1回だけ確認できるようにする（複数回
  スワップする場合、毎回「ドライブ○:と○:を入れ替えました」等が表示されると煩雑なため）。
  スワップが3回以上になる場合も同様に、最後の1回以外は`> NUL`を付けます。

**動作検証で判明した2つの重要な落とし穴**（2026-08-14、この手順を実際に実行して検証した際に発見）:

1. **`DRIVE`はPATH環境変数を自動更新しない**。交換後に元のPATH文字列（交換前のドライブ
   レターを指したまま）で外部コマンドを探すと`コマンドまたはファイル名が違います`になります。
2. **「スワップをPATH設定行より前に置けば安全」という単純な解決策は誤り**。1回目の
   `DRIVE A: B:`実行時点で既に`A:`の意味が変わってしまうため、`PATH`にまだ`A:\BIN;...`が
   残っていると（`DRIVE`実行前に一度でも`PATH A:\...`を発行していても）、**2回目の
   `DRIVE B: C:`が`PATH`経由で見つからず同じエラーになる**（複数回スワップする場合、
   `PATH`の参照先ドライブは1回のスワップごとに古くなっていく）。

   → **確実な回避策は、`DRIVE.X`を`PATH`検索に頼らず`A:\BIN\DRIVE`のようにフルパスで
   直接呼ぶこと**（`docs/x68k-usage/official/DRIVE.X.md`の「PATH破損からの復旧」でも
   同じ手法が使われている）。各呼び出しの直前時点でHDDが実際にどのドライブレターに
   いるかを追いながら、そのレターでフルパス指定する（例の1行目は`A:`＝まだ交換前の
   HDD、2行目は`B:`＝1回目のスワップでHDDが移った先）。全スワップ完了後に初めて
   `PATH`を1回だけ、最終的な位置（例では`C:`）で設定すればよい。
- CONFIG.SYSはこのプロジェクトの構成では相対パス（`\SYS\...`等、ドライブ文字なし）で
  デバイスドライバを指定しているため、ドライブレターが入れ替わっても編集不要
  （Human68kユーザーズマニュアル2.9節のCONFIG.SYS例で確認）。**ただし辞書ドライバ
  `DEVICE = \SYS\ASK68K.SYS /D<d>:\ASK\X68K.DIC ...`はドライブ文字を明示しているため対象外**。
  Step 3-2で辞書をインストール済みの場合、`/D<d>:`を本Stepの最終ドライブレター
  （上の例では`C:`、つまり`/DC:\ASK\X68K.DIC`）に合わせて再度書き換えること（手順は
  Step 3-2参照。Step 3-2の時点ではまだ入れ替え前のレターで書いているため、Step 6-3実施後は
  必ずここでもう一度直す）。

## Step 7: 結果をユーザーに報告する

作成したファイルパス・容量・ボリューム名・転送したファイル数（`copyall`の出力に含まれる）・
メモリ容量変更の有無・`ix68k.cfg`への追記内容・起動装置設定（Step 6-1）・
（Step 3-5を実施した場合）SUSIE.Xの導入とCONFIG.SYSへの`DEVICE=`追記内容・
（Step 3-6を実施した場合）HOSTDRV.SYSの導入と`hostdrv_dir`の設定内容・
（Step 6-3を実施した場合）ドライブレター入れ替えとAUTOEXEC.BATの変更内容をまとめて報告します。

---

## 注意事項

- 本手順はSCSI HDDからの起動が動作するエミュレータ実装を前提にしている
  （`docs/ix68k-internals/design/scsi_design.md`参照。実装・起動確認済み）。
- `hu68disk mkfs --type scsi`はXEiJ `HumanMedia.java`と同じアルゴリズムでクラスタサイズを
  容量に応じて自動選択する（データ領域のクラスタ数を65520以下に保つよう、必要なら
  1クラスタ=16KB/64KB等に拡大する）ため、実機と同様に数百MB〜2GB級のイメージも作成できます。
- SCSIの`Read(6)`コマンドはLBAが21bit固定で512Bブロック換算1GiBまでしかアドレスできないが、
  実際のHuman68kのSCSIドライバ（ディスク側に転送される実バイナリ）は1GBを超えると
  自動的に32bit LBAの`Read(10)`へ切り替えます。ix68kもこれに対応済みで、
  2GBイメージ内の1GB境界を超えた位置のファイルを実際に読み書きできることを
  起動を伴う手順で確認済み（`src/peripheral/scsi.c`）。
- `copyall`はコピー元のファイル属性（system/hidden/readonly）・タイムスタンプを保持します。
  ボリュームラベル・削除済みエントリ・VFAT LFN断片はコピー対象外です。
- **HUMAN.SYSは連続クラスタでなければならない**（Human68kマニュアル`SYS`コマンドの節に明記。
  非連続だと起動不能になりうる）。`tools-host/hu68disk/fat.c`のクラスタ割り当ては単純な先頭からの
  空き探索（バンプアロケータ）で、**Step 2の`mkfs`直後・何も削除されていないバージン領域への
  最初の書き込み**であるStep 3のシステムディスク転送に限っては、書き込み順に関わらず結果的に
  連続配置になります。実機マニュアル2.9節「システム、辞書ディスクの内容を内蔵ハードディスクに
  転送する」ではこの転送に`SYS`コマンドを使わず単純な`COPYALL /S`（HUMAN.SYSを除く）のみで
  済ませている（実機ではHUMAN.SYS自体は前段のHDD初期化＝`FORMAT`の「領域確保」画面の
  「システム転送：する」で書き込まれる）。
- **上記の理由により、本手順は「新規`mkfs`直後・最初の`copyall`」の一回限りに限定すること。**
  既にファイルが入っている`.HDS`に対してHUMAN.SYSを`copyall`で上書き（バージョン更新等）すると、
  `put_file()`が旧クラスタ連鎖を解放してから`write_file_data()`で再割り当てするため、新ファイルが
  旧ファイルより大きい場合に非連続化する可能性がある（`hu68disk`にはこれを検知・警告する機能が
  無い）。HUMAN.SYSを更新したい場合は`mkfs`からやり直すこと。
