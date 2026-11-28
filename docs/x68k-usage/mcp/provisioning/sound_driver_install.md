# AIエージェントによる音源ドライバ一式インストール手順（自然文1つで完結させる）

Claude Code や LM Studio 等、**ホストのシェルコマンドを実行できるAIエージェント**に対し、
次のような自然文を渡すだけで、既存のHuman68k起動ディスク（`.HDS`）へADPCM／Z-MUSIC演奏環境
一式を導入できるようにするための固定手順（レシピ）。

> 音源ドライバ一式をインストールして

導入するのは以下3本（`docs/x68k-usage/README.md`「ソフトウェアバンドルインストール」の
「音源ドライバ一式」バンドル定義に対応する）。

| 順序 | ソフト | マニュアル | 役割 |
|------|--------|-----------|------|
| 1 | PCM8A.X | [../../freeware/PCM8A.X.md](../../freeware/PCM8A.X.md) | リアルタイムADPCM多重再生ドライバ（PCM8＋MPCM相当） |
| 2 | ZMUSIC.X | [../../freeware/ZMUSIC.X.md](../../freeware/ZMUSIC.X.md) | 音源ドライバ Z-MUSIC ver2（FM8音＋ADPCM＋MIDI） |
| 3 | MZP.X 一式 | [../../freeware/MZP.X.md](../../freeware/MZP.X.md) | マルチミュージックプレーヤー本体＋変換ツール5本 |

**この手順はAIエージェントが読んで実行することを想定している**（都度LLMが手順を考案するのでは
なく、毎回同じ手順を辿らせて再現性を確保するのが目的）。人間が手動で行う場合もそのまま使えます。

HDDイメージ自体の作成は本手順のスコープ外（[hdd_install.md](hdd_install.md)
を先に実施すること）。

---

## 前提

| 項目 | 確認方法 |
|------|---------|
| Human68kが起動する`.HDS`を用意済み | 例: `disk/SYSTEM_HD.HDS`。無ければ[hdd_install.md](hdd_install.md)を先に実施 |
| `hu68disk` が使える | リリースZIPでは `tools-host/hu68disk/` にビルド済みの `hu68disk`（Windowsは `hu68disk.exe`）が入っている。ソースから使う場合は `tools-host/hu68disk/` で `make`（`./hu68disk` が生成される） |
| `unar` を導入済み | macOSは `brew install unar`、Linuxはディストリビューションのパッケージ（例: `apt install unar`）。**LZH展開に`lha`を使わない**（Shift-JIS特殊文字を含むファイル名で失敗する） |
| エミュレータをビルド済み | Step 6の起動確認で使います。未ビルドなら[ビルド・実行手順](https://github.com/TerukiYamaguchi/ix68k/blob/main/docs/ix68k-internals/build.md)を参照 |
| （Step 3のパッチ適用時のみ）`BUP.X`が対象HDSの`\BIN`に導入済み | [../../freeware/BUP.X.md](../../freeware/BUP.X.md)。バイナリ差分パッチをX68000上で適用するために必要 |

**hu68diskによるHDS書き込みはエミュレータ停止中に行うこと**（起動中に書き換えると
エミュレータ側のキャッシュと不整合を起こす）。

**コマンドのパスについて**: コマンド例は、リポジトリまたはリリースZIPの展開先をカレントディレクトリとし、
`tools-host/hu68disk/hu68disk` の相対パスで書いている。[emulator_install.md](emulator_install.md)で導入した
ディレクトリ（`hu68disk` を直下へコピーした構成）で実行する場合は、`<Q1のディレクトリ>` に`cd`した上で
`./hu68disk` に読み替える（詳細は[hdd_install.md](hdd_install.md)の「コマンドのパスについて」参照）。

---

## Step 1: ユーザーに質問する

**1問ずつ**聞く（複数まとめて聞かない）。

### Q1. 対象の`.HDS`

> どのディスクイメージへインストールしますか？（既定: `disk/SYSTEM_HD.HDS`）

- 未回答・「おまかせ」等なら `disk/SYSTEM_HD.HDS` を採用します。
- 以降の手順の`<HDS>`をこの回答で置き換えます。ドライブレターが`C:`でない場合は
  Step 5のパス表記も読み替えること。

### Q2. AUTOEXEC.BATへの常駐登録

> PCM8A・ZMUSICを起動時に自動で常駐させますか？（既定: はい。常駐するとPCM8Aが標準
> 370〜400KB、ZMUSICがトラックバッファ128KB＋ADPCMバッファ256KB＋本体分の空きメモリを
> 消費します）

- 「はい」ならStep 5を実施します。「いいえ」ならStep 5を飛ばし、使用時に手動で
  `PCM8A` → `ZMUSIC` の順に実行する旨を報告します。

---

## Step 2: PCM8A.X を導入する

配布元: http://retropc.net/x68000/software/sound/adpcm/pcm8a/ （X68000 LIBRARY、version 1.02）。

```sh
# 1. ダウンロード（arc/ に既にあればスキップする。配布元サーバへの再アクセスを避ける）
curl -L --create-dirs \
  -o "arc/retropc.net/x68000/software/sound/adpcm/pcm8a/pcm8a102.lzh" \
  "http://retropc.net/x68000/software/sound/adpcm/pcm8a/pcm8a102.lzh"

# 2. 展開
unar -e SHIFT-JIS -o ./pcm8a_extracted \
  "arc/retropc.net/x68000/software/sound/adpcm/pcm8a/pcm8a102.lzh"
# → ./pcm8a_extracted/pcm8a102/{PCM8A.X, PCM8A.DOC, PCM8ATEC.DOC, PCM8AVUP.DOC, PCM8A102.HAS}

# 3. 本体を \BIN へ
tools-host/hu68disk/hu68disk copy <HDS> ./pcm8a_extracted/pcm8a102/PCM8A.X /BIN

# 4. ドキュメントを \DOC\PCM8A\ へ（ソース PCM8A102.HAS は転送しない）
tools-host/hu68disk/hu68disk mkdir <HDS> /DOC/PCM8A
for f in PCM8A.DOC PCM8ATEC.DOC PCM8AVUP.DOC; do
  tools-host/hu68disk/hu68disk copy <HDS> "./pcm8a_extracted/pcm8a102/$f" /DOC/PCM8A
done
```

---

## Step 3: ZMUSIC.X を導入する

配布元: http://retropc.net/x68000/software/sound/zmusic/zmusic2/ （X68000 LIBRARY）。

```sh
# 1. ダウンロード（arc/ZMSC208X.LZH・arc/zm208e.lzh に既にあればスキップ）
curl -L -o arc/ZMSC208X.LZH \
  "http://retropc.net/x68000/software/sound/zmusic/zmusic2/ZMSC208X.LZH"

# 2. 展開
unar -o ./zmusic_x_extracted arc/ZMSC208X.LZH
# → ./zmusic_x_extracted/ZMSC208X/{ZMUSIC.X, ZMSC.X, ZPCNV.R, ZP.R, ZVT.X, ...}

# 3. 本体を \BIN へ（同梱ツールZMSC.X/ZPCNV.R/ZP.R/ZVT.Xも同様にコピーしてよい）
tools-host/hu68disk/hu68disk copy <HDS> ./zmusic_x_extracted/ZMSC208X/ZMUSIC.X /BIN
```

### Step 3-2（任意）: 2.08e パッチを適用する

68040/68060のコピーバックキャッシュ対応版（2.08e）にする場合のみ。差分パッチ`zm208e.lzh`を
`BUP.X`で当てます。**`bup.x`はX68000実行ファイルのためホスト側では動かせず、エミュレータ内で
実行する必要がある**。手順の詳細（`-NT`が必要になる条件、パッチ後に旧版が常駐していると
`-R`が失敗する件を含む）は
[../../freeware/ZMUSIC.X.md](../../freeware/ZMUSIC.X.md)
「2.08e パッチ適用手順」を参照。

パッチを当てない場合も2.08として動作する（バナーが`VERSION 2.08`のままになるだけ）。

---

## Step 4: MZP.X 一式を導入する

**配布元URLが不明**なため、リポジトリ内アーカイブ
`arc/x68tools/SOUND/ZMUSIC/MZP/MZP04B_T.LZH`（rel.4bテスト版）を使います。
無い場合はユーザーに入手方法を確認すること（記憶で配布元URLを埋めない）。

```sh
unar -e SHIFT-JIS -o ./mzp_extracted arc/x68tools/SOUND/ZMUSIC/MZP/MZP04B_T.LZH
# → ./mzp_extracted/MZP04B_T/{MZP.x, MXtoZ.x, NDtoZ.x, RCtoZ.x, MLtoZ.x, SDtoZ.x, README}

for f in MZP.x MXtoZ.x NDtoZ.x RCtoZ.x MLtoZ.x SDtoZ.x; do
  tools-host/hu68disk/hu68disk copy <HDS> "./mzp_extracted/MZP04B_T/$f" /BIN
done
```

MZPはZ-MUSIC ver2.00以上が常駐していることが前提の**非常駐**プレーヤーなので、
AUTOEXEC.BATには登録しない（演奏時にコマンドラインから起動する）。

MDF/ZDF形式（圧縮された演奏データ）を扱う場合は別途`lzz.r`が必要（**未導入**、
必要になった時点で追加する）。

---

## Step 5: AUTOEXEC.BAT へ PCM8A / ZMUSIC を登録する（Q2が「はい」の場合のみ）

**`PCM8A`を`ZMUSIC`より前の行に置くこと。** Z-MUSICがADPCMを多重発声するには、PCM8系
ドライバが先に常駐している必要がある（正しい順序なら起動時に
`ADPCM is managed by PCM8.X (C)H.ETOH` と表示される）。

**編集後は必ずCRLF改行で保存する**（LFのまま書き戻すとHuman68k側で表示が崩れる／一部ツールが
正しく解釈しない可能性があります。[hdd_install.md](hdd_install.md)の
CRLF注意も参照）。追記位置は**EOFマーカー`$1A`の直前**。

```sh
tools-host/hu68disk/hu68disk extract <HDS> /AUTOEXEC.BAT ./AUTOEXEC.BAT.tmp

python3 - <<'PY'
data = open("./AUTOEXEC.BAT.tmp", "rb").read()
assert b"PCM8A" not in data and b"ZMUSIC" not in data, "既に登録済み"
i = data.rfind(b"\x1a")            # EOFマーカーの直前に挿入する
assert i != -1, "EOFマーカーが見つからない"
open("./AUTOEXEC.BAT.tmp", "wb").write(data[:i] + b"PCM8A\r\nZMUSIC\r\n" + data[i:])
PY

tools-host/hu68disk/hu68disk copy <HDS> ./AUTOEXEC.BAT.tmp /AUTOEXEC.BAT
```

書き戻し後の末尾は次のようになる（`hu68disk extract <HDS> /AUTOEXEC.BAT /dev/stdout | cat -v`
で確認できます。`^M`=CR、`^Z`=EOFマーカー）:

```
SET MAKE_SHELL=COMMAND.X^M
^M
PCM8A^M
ZMUSIC^M
^Z
```

### Step 5-2: CONFIG.SYS から OPMDRV3 の行を削除する

`OPMDRV3.X`はFM音源(OPM)を占有する音源ドライバで、OPMを制御するZMUSICと併用しません。
CONFIG.SYSに登録が残っていたら**行ごと削除する**（`;`でコメントアウトでもよいが、
本手順では削除で統一する）。削除対象の行は次の形をしている:

```
DEVICE    = ¥SYS¥OPMDRV3.X #180 /P64 /OPM
```

```sh
tools-host/hu68disk/hu68disk extract <HDS> /CONFIG.SYS ./CONFIG.SYS.tmp
# CRLFを保ったままOPMDRV3を含む行だけ落とす（Pythonワンライナー等で）:
#   data = open("./CONFIG.SYS.tmp","rb").read()
#   kept = [ln for ln in data.split(b"\r\n") if b"OPMDRV3" not in ln.upper()]
#   open("./CONFIG.SYS.tmp","wb").write(b"\r\n".join(kept))
tools-host/hu68disk/hu68disk copy <HDS> ./CONFIG.SYS.tmp /CONFIG.SYS
```

改行はCRLFのまま保つこと（行の分割・結合を`\r\n`で行えば維持される）。EOFマーカー`$1A`は
最終行に含まれたまま残るので、AUTOEXEC.BATのような挿入位置の考慮は不要です。

削除後、起動時メッセージからOPMDRV3の常駐メッセージ（`\SYS\OPMDRV3.X`が存在しない場合は
`¥SYS¥OPMDRV3.Xが登録できませんでした`）が消えることをStep 6で確認します。

---

## Step 6: 起動して常駐バナーを確認する

エミュレータを起動し、スクリーンショットで以下の表示を確認します。

```
A>ECHO OFF
X68k PCM8A polyphonic ADPCM driver v1.02 (c)1993-97 philly
Z-MUSIC UNIVERSAL VERSION 2.08e ... ZENJI SOFT
Secure 128kByte(s) for track buffer.
Secure 256kByte(s) for ADPCM data buffer.
ADPCM is managed by PCM8.X (C)H.ETOH
MIDI/FM・OPM/ADPCM are under the control of ZMUSIC.
C:¥>
```

チェックポイント:

1. PCM8Aのバナーが**ZMUSICより先に**出ていること。
2. `ADPCM is managed by PCM8.X` が出ていること（出ない場合はPCM8Aが常駐していないか、
   常駐順序が逆）。
3. プロンプト（`C:¥>`）まで到達すること。

エラー画面が出た場合の切り分け順序:

1. AUTOEXEC.BATを変更前に戻して再起動し、変更起因かを確認します。
2. 変更起因なら、常駐行を1行ずつ減らして原因のドライバを特定します。

---

## Step 7: MMDSPも導入するか確認する

音源ドライバ一式の導入が終わったら、**最後にMMDSPを入れるか聞く**。

> 演奏状態を表示するステータスディスプレイ兼セレクタのMMDSPも入れますか？（常駐した
> ドライバの演奏状態を鍵盤・スペアナ付きで表示し、ファイルセレクタから曲を選べます）

- 「はい」なら[../../freeware/MMDSP.R.md](../../freeware/MMDSP.R.md)の
  「インストール」に従って導入する（`\BIN`へ本体、`\DOC\MMDSP\`へドキュメント、
  BUP.Xでpatchlevel 2を適用）。導入後、`MMDSP`を起動してコントロールパネルの
  DRIVER欄・ADPCM欄に`ZMUSIC`・`PCM8A`が入ることを確認し、`[ESC]`で終了します。
- 「いいえ」ならStep 8へ進みます。

MMDSPは音源ドライバ一式バンドルには**含めない**（演奏環境そのものではなく表示・選択用の
フロントエンドのため、Step 6までで演奏環境としては完結している）。

---

## Step 8: 結果をユーザーに報告する

対象`.HDS`・導入した実行ファイルと配置先（`\BIN`、`\DOC\PCM8A\`）・ZMUSICのバージョン
（2.08 か 2.08e か）・AUTOEXEC.BATへの追記内容・CONFIG.SYSから削除したOPMDRV3の行・
起動確認で観測したバナー・
（Step 7で導入した場合）MMDSPのバージョンと起動確認結果をまとめて報告します。
未実施の項目（パッチを当てなかった、MZPのアーカイブが無く導入できなかった等）は明示します。

---

## 注意事項

- **常駐順序はPCM8A → ZMUSIC固定**。逆順ではZ-MUSICがADPCMを自前管理し、PCM8互換の
  多重再生が効きません。
- **PCM8Aの常駐サイズは標準370〜400KB**と大きい（`PCM8A.DOC`基本機能12)）。空きメモリを
  優先する場合は`-S`でチャンネル数・処理バイト数を絞る（例: `PCM8A -S16`）。ただし常駐時の
  全チャンネル数は25未満にできません。
- PCM8AはCONFIG.SYSから`DEVICE=`で組み込むこともできる（デバイス名は`PCM`、`-N`指定で
  `@PCM`）。その場合AUTOEXEC.BATへの登録は不要。本手順ではAUTOEXEC.BAT方式のみ検証済み。
- PCM8Aは音質より速度を優先した実装で、`PCM8`／`MPCM`より音質が低下する（`PCM8A.DOC`
  注意事項1)）。音質重視なら`PCM8`または`MPCM`を使います。
- ダウンロードした配布アーカイブは`arc/`へ保存し、再インストール・再検証時は配布元へ
  再アクセスせずそちらを使う。
- 各ソフトのスイッチ・操作の詳細は上表のマニュアルを参照。本手順はインストールと
  起動時常駐までを対象とし、演奏データの再生手順は含みません。

---

## 検証状態

- 2026-08-18、`disk/SYSTEM_HD.HDS`に対しStep 2・Step 5・Step 6を実施し、
  PCM8A v1.02とZ-MUSIC 2.08eの常駐、`ADPCM is managed by PCM8.X`表示、
  `C:¥>`プロンプト到達までを確認済み。
- Step 3（ZMUSIC本体の導入と2.08eパッチ適用）は2026-08-06に、Step 4（MZP一式）は
  2026-08-08に実施済み。**新規`.HDS`に対しStep 2〜6を最初から通しで実行した検証は未実施**。
- Step 7（MMDSP）は2026-08-18に実施済み。v0.29 Rel.31 patchlevel 2の導入と起動で、
  DRIVER欄`ZMUSIC`・ADPCM欄`PCM8A`の自動認識、`[ESC]`での終了までを確認。
