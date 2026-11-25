---
exe: ZMUSIC.X
種別: freeware
作者: Z.Nishikawa さん（原作）/ MZL さん（現行メンテナ）
版: v2.08e（ZMSC208X.LZH収録のv2.08に`zm208e.lzh`差分パッチ適用済み、56992 bytes）
出典: 配布元 https://retropc.net/x68000/software/sound/zmusic/zmusic2/ （X68000 LIBRARY）
判別: -（配布アーカイブ ZMSC208X.LZH + zm208e.lzh 適用版のみ確認）
検証状態: ✅ インストール・パッチ適用・起動確認済み（起動バナー "UNIVERSAL VERSION 2.08e"）。
  同梱ツール ZMSC.X/ZPCNV.R/ZP.R/ZVT.X も2.08eパッチまで適用しインストール済み（起動確認は未）
最終更新: 2026-08-19
---
# ZMUSIC.X (Z-MUSIC ver2)

> 音源ドライバ Z-MUSIC システム ver2。FM音源8音とADPCM（PCM8.X使用時は同時8音発声可能）、MIDI音源を同時に駆使して演奏できる常駐型サウンドドライバ。Z-MUSIC ver3とは非互換。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式:

```
ZMUSIC [オプション]
```

常駐型ドライバ。オプションなしで実行すると既定設定（内蔵FM音源8音、トラックバッファ128KB、
ADPCMバッファ256KB）で常駐します。スイッチは`/`または`-`の後ろに1文字と、必要なら数値等の
パラメータを続ける形で指定する（例: `zmusic -b drum.zpd -w30 -p100 -s music.zmd`）。複数
指定可・順不同（`-C`系を除く）。`-h`または`-?`で簡易ヘルプを表示します。

常駐時（組み込み時）に使うスイッチ:

| オプション | 効果 |
|-----------|------|
| `-A` | 割り込みにタイマAを使う（既定はタイマB）。テンポ77〜300での精度が高く、外部シーケンサ／リズムマシンとの同期に向く |
| `-B<ファイル名>` | ADPCMブロックデータ(ZPDデータ)を起動時に読み込む（拡張子省略時`.ZPD`）。`-P`未指定ならこのデータサイズがそのままADPCMバッファサイズになる |
| `-E` | 外部シーケンサ／リズムマシンと同期演奏する（MIDI同期メッセージ$FA/$FB/$FC/$F8を送信） |
| `-G` | 起動時のロゴ・常駐報告メッセージを表示しない |
| `-M` | MFP多重割り込み対応モードにする（ラスタースクロール等の妨げにならない） |
| `-N` | 初期化なしモードにする（演奏データが切り替わるときに楽器・ワークを初期化しない） |
| `-O[n]` | PCM8.X使用時、ADPCM1声の曲データを強制的にポリフォニック化します。`n`(1〜8、省略時8)は音楽演奏用に確保するチャンネル数 |
| `-P<n>` | ADPCMデータバッファを`n`KB確保する（既定256、`-P0`でADPCM関連コマンドを無効化） |
| `-R` | 常駐中のZMUSIC.Xを削除する（コマンドライン常駐時のみ有効。CONFIG.SYSの`DEVICE=`組み込みでは不可） |
| `-S<ファイル名>` | セットアップファイル（MIDIダンプ／ADPCMコンフィグ等）を読み込んで起動時に実行します。拡張子省略不可、MIDIダンプは`.MDD`のみ有効。演奏データファイルも指定でき、その場合は起動時にその曲を演奏開始する |
| `-T<n>` / `#n` | トラックバッファを`n`KB確保する（既定128KB） |
| `-W<n>` | ワークエリアを`n`KB確保する（ADPCM加工・コンフィグバッファ・MIDI楽器設定取込・波形メモリに使う） |
| `-X<n>` | EOX送信後に`n/60`秒待つ（既定3。応答の遅いMIDI楽器向け） |

MML→ZMDコンパイルを支援するスイッチ:

| オプション | 効果 |
|-----------|------|
| `-C <in> [out]` | `<in>`（MML/OPM）をコンパイルし`[out]`へ保存する（省略時は`<in>`の拡張子を`.ZMD`に変える）。**コマンドラインの最後に置く必要がある**（それより後のスイッチは無視される）。大文字`-C`は空トラック削除の最適化を行わない |
| `-Q <in> [out]` | コンパイル後の演奏データの総ステップ数を計算する（`[do]`〜`[loop]`以外の書き方で無限ループを作ると計算が終わらない点に注意。雑誌投稿時のチェックサム的な用途） |
| `-T<n>` / `#n` | コンパイル用トラックバッファを`n`KB確保する（既定128KB、`out of memory`が出たら調整） |
| `-W<n>` | コンパイル用の整理エリアを`n`KB確保する（既定128KB） |

CONFIG.SYSからの組み込み:

```
DEVICE=ZMUSIC.X [オプションスイッチ…]
DEVICE=ZMUSIC.X -s<曲ファイル名>    ; 組み込み時から曲を再生
```

### 出力先の違いによる3タイプ

UNIVERSAL版（シャープ純正/互換MIDI I/F対応、標準）・RS232C版（RS232C-MIDIアダプタ経由、
外部同期`-E`なし、処理速度がやや遅い）・POLYPHON版（ネオ・コンピュータ・システム製
サブボード「ポリフォン」経由、`PCM8SB.X`の先行組み込みが必要、外部同期`-E`なし）の3種類が
あり、内蔵音源（FM/ADPCM）を制御する部分は共通でMIDI出力部分だけが異なります。X68000/X68030
両対応。今回インストールしたのはUNIVERSAL版（起動バナーで確認済み）。

### ZMSC.X（機能縮小版）との相違点

MMLコンパイル処理を省いたZMD再生専用版。常駐サイズはZMUSIC.Xの約半分でZMS再生はできません。
ファンクション`$01 m_alloc`/`$02 m_assign`/`$03 m_vget`/`$06 m_trk`/`$07 m_free`/
`$3e set_fm_master_vol`は無効化されています。既定値もZMUSIC.Xと異なる（トラックバッファ
32KB、ワークエリア0KB、ADPCMバッファ256KB）。

---

## ix68kでの利用

### 入手先

https://retropc.net/x68000/software/sound/zmusic/zmusic2/ から以下3アーカイブをダウンロード可能です。

- `ZMSC208X.LZH` … 実行ファイル（`ZMUSIC.X` `ZMSC.X` `ZPCNV.R` `MUSICZ.FNC` `ZP.R` `ZVT.X` `ZPLK.R` 等）
- `ZMSC208S.LZH` … ソースコード（`ZMSC.HAS` 等、HAS/S形式）
- `ZMSC2MAN.LZH` … マニュアル（`ZM0.MAN`〜`ZM16.MAN`等、テキスト形式）
- `zm208e.lzh` … 2.08→2.08e バイナリ差分パッチ（68040/68060コピーバックキャッシュ対応。**適用済み**、[BUP.X](BUP.X.md)で適用）

**`arc/ZMSC208X.LZH` `arc/ZMSC208S.LZH` `arc/ZMSC2MAN.LZH` `arc/zm208e.lzh`にローカル保存済み**
（2026-08-06取得）。再検証・再インストール時はretropc.netへ再度アクセスせず、まずこちらを使うこと。

ソースコード・マニュアルは`extern/zmusic/`に展開済み（gitignore対象、参照用）。
マニュアル（`ZMSC2MAN.LZH`）はファイル名にShift-JIS文字を含むため、展開時は
`unar -encoding Shift_JIS`を指定すること（省略すると`ZM14図１.PIC`
`用語解説.MAN`が文字化けする）。

### インストール（SYSTEM_HD.HDSへの導入手順）

```sh
# 1. ダウンロード（arc/ZMSC208X.LZHにローカル保存済みならそれを使う）
cp arc/ZMSC208X.LZH ./ZMSC208X.LZH
# 無ければ: curl -LO https://retropc.net/x68000/software/sound/zmusic/zmusic2/ZMSC208X.LZH

# 2. 展開（unar、要 brew install unar）
unar -o ./zmusic_x_extracted ZMSC208X.LZH
# → ./zmusic_x_extracted/ZMSC208X/{ZMUSIC.X, ZMSC.X, ZVT.X, ...}

# 3. ZMUSIC.XをHDSの\BINへコピー（hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./zmusic_x_extracted/ZMSC208X/ZMUSIC.X /BIN

# 4. AUTOEXEC.BATへ常駐登録（オプションなし、末尾に追記）
tools-host/hu68disk/hu68disk extract disk/SYSTEM_HD.HDS /AUTOEXEC.BAT ./AUTOEXEC.BAT.tmp
# 末尾（EOFマーカー$1Aの直前）に "ZMUSIC.X\r\n" を追記してから書き戻す
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./AUTOEXEC.BAT.tmp /AUTOEXEC.BAT
```

### 2.08e パッチ適用手順（[BUP.X](BUP.X.md)使用）

`bup.x`はコピー元ファイルとタイムスタンプが一致しないと`Error: タイムスタンプが違います`で
拒否します。**アーカイブ（unar）から直接抽出したファイルをそのままhu68disk copyで転送する分には
配布時オリジナルのタイムスタンプが保持されるため`-NT`は不要**（`hu68disk copy`はホストファイルの
mtimeをそのままFAT日時へ書き込む実装。2026-08-06、ZMSC.X/ZPCNV.R/ZP.R/ZVT.Xの4ファイルとも
`-NT`なしで成功）。`-NT`が必要になるのは、`hu68disk extract`でHDSから一度ホストへ取り出した
ファイルを使う場合など、タイムスタンプがどこかで失われた場合のみ。
ix68k内で実行する必要がある（`bup.x`はX68000実行ファイルのためホスト側では
動かせない）。

```sh
# 1. ホスト側: HDS内の作業ディレクトリへZMUSIC.XとZMUSIC.BFDを配置（エミュレータ停止中）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /TMP
tools-host/hu68disk/hu68disk extract disk/SYSTEM_HD.HDS /BIN/ZMUSIC.X ./ZMUSIC_OLD.X
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./ZMUSIC_OLD.X /TMP/ZMUSIC.X
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS <zm208e展開先>/ZMUSIC.BFD /TMP
# bup.xは /BIN に既にインストール済みならそちらを使う（PATH経由）
```

```
# 2. エミュレータ起動後、ix68k内で（MCP run_command等）
CD \TMP
BUP ZMUSIC.BFD -NT
# → `ZMUSIC.X' ... completed. （元ファイルは自動でTMP\origへバックアップされる）
COPY ZMUSIC.X C:\BIN\ZMUSIC.X
```

パッチ後は常駐中の旧バージョンが残っていると`-R`（常駐解除）が
`Illegal version number. Unable to release.`で失敗する（バージョン不一致チェック、異常ではない）。
リブートしてAUTOEXEC.BAT経由で新バージョンを再ロードすれば正常に常駐します。
作業完了後は`/TMP`をhu68diskで削除します。

### 検証メモ

- 検証: 2026-08-18、[PCM8A.X](PCM8A.X.md)常駐後にZMUSIC.Xを常駐させる構成へ変更
  （AUTOEXEC.BATに`PCM8A`→`ZMUSIC`の順で登録）。起動時に
  "ADPCM is managed by PCM8.X (C)H.ETOH" と表示され、ADPCMがPCM8系ドライバ管理下に
  入ることを確認（track buffer 128KB / ADPCM data buffer 256KB確保、エラーなし）。
- 検証: 2026-08-06、ix68k起動でAUTOEXEC.BAT経由の常駐を確認。
  - v2.08（パッチ前）: 起動時「MIDI/FM・OPM/ADPCM are under the control of ZMUSIC.」まで
    正常表示、エラーなし。
  - `bup.x -NT`でZMUSIC.BFD適用 → `\BIN\ZMUSIC.X`を56310→56992 bytesの2.08eへ更新。
  - リブート後の起動バナーが「UNIVERSAL VERSION 2.08e」「19/Mar/2003 modified by MZL.」に
    変化、Secure/track buffer・ADPCM data bufferの確保メッセージも正常表示。エラーなし。
  - **結論: インストール・2.08eパッチ適用とも正常動作を確認済み。**
- 検証: 2026-08-06、同梱ツール4点（ZMSC.X/ZPCNV.R/ZP.R/ZVT.X）も同様の手順でv2.08を
  `\BIN`へインストールし、対応する`.BFD`（ZMSC.BFD/ZPCNV.BFD/ZP.BFD/ZVT.BFD）を
  `bup.x`（オプションなし、`-NT`不要）で適用。DIRで全ファイルのサイズ増加・日付更新
  （ZMSC.X 36036→36574 bytes、ZPCNV.R 6908→6966 bytes、ZP.R 11110→11208 bytes、
  ZVT.X 34390→34412 bytes）を確認しエラーなく`\BIN`へ上書き完了。個別の起動確認は未実施。
