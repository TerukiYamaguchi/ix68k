---
exe: -（060turbo 関連ツール集。060cache.X / 060dmam.X / 060dmap.X / 060high.X / 060loadhigh.X / 060mat.X / 060mmu.X / 060opmp.X / 060pcr5.X / 060prni.X / 060prnp.X / 060ps.X / 060simck.sys / exx.X）
種別: freeware
作者: 鎌田 誠 (Makoto Kamada)
版: 060cache v0.82 / 060dmam v0.02 / 060dmap・060opmp・060prnp v0.01 / 060high v0.10 / 060loadhigh v0.11 / 060mat v1.10 / 060mmu v0.10 / 060pcr5 / 060prni v0.01 / 060ps v0.10 / 060simck v0.12 / exx v1.00
出典: 各アーカイブ添付の .doc。配布元 http://retropc.net/x68000/software/hardware/060turbo/ （X68000 LIBRARY「060turbo 関連」）
判別: -（配布アーカイブ 060CACHE.LZH / 060DMAM2.LZH / 060PP001.LZH / 060HIGH.LZH / 060lh011.lzh / 060MAT.LZH / 060MMU.LZH / 060PCR5.LZH / 060PI001.LZH / 060PS.LZH / 060SC012.LZH / EXX100.LZH 収録版のみ確認）
検証状態: 060mmu.X のみ ✅（ix68k で起動・出力確認）。他は 📖 マニュアル参照のみ。SYSTEM_HD.HDS の `\BIN\060turbo` へ導入・PATH 登録済み
最終更新: 2026-10-28
---
# 060turbo 関連ツール集

> 060turbo（MC68060 アクセラレータ）用の補助ツール群。060turbo.sys 本体は含まない（別途導入済み）。

---

## リファレンス（一次資料要約）

### ツール一覧

| 実行ファイル | 役割 | 常駐 |
|-------------|------|------|
| 060cache.X | ページ毎のキャッシュモードの表示・変更 | - |
| 060dmam.X | 060turbo.sys v0.54 以前の DMA 系 IOCS の不具合修正 | ✔ |
| 060dmap.X | DMAC レジスタアクセス時にデータキャッシュを自動プッシュ | ✔ |
| 060high.X | プログラムのロード先・メモリ確保先の領域制限 | - |
| 060loadhigh.X | 指定プログラムをハイメモリへ強制ロードして実行 | - |
| 060mat.X | MATIER（MAT.X）へのダイナミックパッチ起動 | - |
| 060mmu.X | アドレス変換（MMU）状態の表示 | - |
| 060opmp.X | FM 音源ポートのアクセスにウェイトを追加 | ✔ |
| 060pcr5.X | store/load bypass の禁止・許可（エラッタ回避） | - |
| 060prni.X | 060turbo + Mach-2 でのプリンタ割り込み不具合回避 | ✔ |
| 060prnp.X | プリンタポートのアクセスにウェイトを追加 | ✔ |
| 060ps.X | メモリ管理状態の表示（PROCESS.X の代替） | - |
| 060simck.sys | SIMM チェッカ（起動時のみ動作） | - |
| exx.X | 分岐キャッシュ等 68060 機能の ON/OFF | - |

### 060cache.X

書式: `060cache [w|c|s|n]`（数字指定は `0|1|2|3`）。引数なしでページ毎のキャッシュモード一覧を表示します。
引数ありのときは、060cache.x 自身がロードされたメモリブロックを可能な限り延長し、その範囲のモードを変更します。

| 指定 | 内容 |
|------|------|
| `w` / `0` | キャッシュ許可・ライトスルー |
| `c` / `1` | キャッシュ許可・コピーバック |
| `s` / `2` | キャッシュ禁止・ストアバッファ禁止 |
| `n` / `3` | キャッシュ禁止・ストアバッファ許可 |

ハイメモリ上で動かすプログラムのモードを変えるときは、060cache.x も 060loadhigh.x 経由でハイメモリ上で動かす
（例: `060loadhigh 060cache w`）。

### 常駐型ツール共通（060dmam / 060dmap / 060opmp / 060prni / 060prnp）

いずれも 060turbo の 68060 モードで動作し、操作体系は共通。`<name>` は各ツール名。

| 操作 | 書式 |
|------|------|
| デバイスドライバとして常駐 | CONFIG.SYS に `DEVICE = <name>.x` |
| コマンドラインで常駐 | `<name>.x` |
| 機能停止状態で常駐 | `DEVICE = <name>.x off` または `<name>.x off` |
| 常駐後の ON/OFF | `<name>.x on` / `<name>.x off` |
| 常駐解除 | `<name>.x -r` |

各ツールの機能:

- **060dmam**: 060turbo.sys v0.54 以前では DMA 系 IOCS（`_DMAMOVE`/`_DMAMOV_A`/`_DMAMOV_L`/`_DMAMODE`）で、
  入力 d1.b のビット7が1のときビット0〜1と2〜3の解釈が逆になってきました。`-sd` スイッチ指定時に誤動作しうるため、
  該当ルーチンを正しいものに差し替えます。060turbo.sys が修正済みなら不要です。
- **060dmap**: MMU で DMAC レジスタ領域（$00E84000〜$00E840FF）に一時的なページフォルトを起こし、
  チャンネルコントロールレジスタアクセスの前にデータキャッシュをプッシュします。コピーバックモードで
  ADPCM が鳴らない古い音源ドライバ等への対策。read/write とも前後合わせて実測約6μs遅延します。
  転送開始以外のアクセス（継続・中止）でもプッシュします。バスマスタ（Mach-2 等）の転送は対象外です。
- **060opmp**: FM 音源アドレスポート($00E90001)・データポート($00E90003)のアクセスに約6μsのウェイトを入れます。
  古い音源ドライバで音が外れる・鳴らない場合の対策。X68030 のハードウェアウェイトは、実測ではアドレスレジスタ
  書き込み時の約1.2μsのみとされます。
- **060prnp**: プリンタデータポート($00E8C001)・ストローブポート($00E8C003)のアクセスに約6μsのウェイトを入れます。
  IOCS を使わないプリンタ出力にも有効です。
- **060prni**: 060turbo の ROM と Mach-2 の相性問題で、プリンタ割り込み（ベクタ$63）を使うプログラムが動かなくなる
  不具合を回避します。`_PRNINTST` の「使用中」判定条件を緩めます。

060dmap / 060opmp / 060prnp はアクセスフォルトベクタを使うためデバッガと共存できません。デバッガ起動前に必ず
`off` にすること（常駐解除は不要）。ウェイトを入れたまま起動すると、対象ポートがデバッガ上で扱えない、
ユーザモード/MOVES でのアクセスがハングする等の影響が出ます。対象アドレス範囲に他の増設ポートがあるとそれも
アクセス不能になります。

### 060high.X

書式: `060high ロード領域制限 [アロケート領域制限]`。DOS コールで確保できるメモリ空間を一時的に制限します。
アロケート側を省略するとロード側と同じになります。起動時は両方 0。

| 番号 | 内容 |
|------|------|
| 0 | 下位（本体メモリ）のみ（既定） |
| 1 | 上位（ハイメモリ）のみ |
| 2 | 親と同じ側のみ |
| 3 | 親と反対側のみ |
| 4 | 制限なし・下位優先 |
| 5 | 制限なし・上位優先 |
| 6 | 制限なし・親と同じ側優先 |
| 7 | 制限なし・親と反対側優先 |
| 8 | 制限なし・優先なし |

「親」はロード時は親プロセス、アロケート時は確保しようとするプロセス。優先側が不足なら 8 と同じ動作。
060loadhigh.x を使うとロードだけ 5 になります。

### 060loadhigh.X

書式: `060loadhigh <プログラム> [引数]`。指定プログラムを強制的にローカルメモリ（ハイメモリ）で動かします。
プログラムによってはハイメモリ上で正常動作しません。

### 060mat.X

MATIER（MAT.X）に 060turbo 用のハイメモリ・DMA 対策パッチをあてて起動します。対応は MAT.X Version 2.0
（93/10/20、476592 bytes）と 2.11B（95/07/17、497620 bytes）。推奨環境は 060turbo + 32MB SIMM、060turbo.sys v0.53 以上。

- 起動方法1: 060mat.x を MAT.X と同じディレクトリに置き、MAT.X の代わりに 060mat.x を実行（ハイメモリで動かすなら先に `060high 1`）。
- 起動方法2: `ren MAT.X orgMAT.X` → `BIND 060mat.x orgMAT.X` → `ren 060mat.x MAT.X`。以降は MAT.X の起動だけでパッチ済みで動きます。
- 仮想画面を4096×4096まで使うには36MB程度の連続空き領域が必要。SIMM 32MB なら 060turbo.sys に `-mm` 等を指定する
  （例: `DEVICE = 060turbo.sys -cm1 -dv -md1ma -mm3 -sd -ss -xm`）。
- 拡大・縮小処理の修正のため、従来と結果が微妙に異なる場合があります。68030モードでも 060mat.x 経由ならパッチが当たる。

### 060mmu.X

書式: `060mmu`（パラメータなし）。060turbo.sys のアドレス変換状態を表示します。

| 欄 | 内容 |
|----|------|
| Logical Address | 論理アドレス範囲 |
| Physical Address | 物理アドレス範囲 |
| User/Super | スーパーバイザプロテクトの有無（Supervisor / User） |
| Cache-mode | Write-through / Copyback / Serialized（キャッシュ禁止・ストアバッファ禁止）/ Non-serialized（キャッシュ禁止・ストアバッファ許可） |
| Write-protect | Write-protected のとき書き込み禁止 |

### 060pcr5.X

書式: `060pcr5 1`（store/load bypass 禁止）/ `060pcr5 0`（許可）。
68060 の F43G / G65V マスクセットには、キャッシュ禁止の I/O ポートにロングワードで書いた直後に同アドレスを読むと、
ポートからではなく書き込んだ値が返ることがあるエラッタがあります。bypass を禁止すると回避できます。X68030 では影響を受ける
アクセスは少ない（VRAM 以外の I/O はふつうロングワードで触らない）。68060 を使わない環境では無意味。

### 060ps.X

書式: `060ps [l][v]`（`l`: 冗長表示、`v`: ベクタ表示）。拡張モードで PROCESS.X が使えないため用意されたメモリ管理状態の表示。
通常表示は 先頭 / 末尾+1 / 長さ / 実行ファイル名 の4欄。冗長表示では ヘッダ / 直前 / 親 / 空き / 直後 を加えた9欄
（親の最上位バイトが$FFなら常駐プロセス）。`v` はそのブロックを指す例外ベクタ（0000〜00FF）・IOCS（0100〜01FF）・
DOS コール（FF00〜FFFF）・各種ワークエリアの番号も表示します。バックグラウンドスレッドの表示はできません。

### 060simck.sys

060turbo 用 SIMM チェッカ。CONFIG.SYS で 060turbo.sys より前に `DEVICE = 060simck.sys` と登録します。常駐せず、
起動時に SHIFT キーを押していると検査する（SIMM 上のデータを破壊するため確認を求めます。Y またはリターンで続行）。
約10秒で終了し、異常データがあれば ERROR、なければ OK を表示します。簡易検査のため OK でも異常を見逃すことがあります。

| スイッチ | 内容 |
|---------|------|
| `-F` | キーが押されていなくても検査する |
| `-Y` | データ破壊の確認を省略する |
| `-K key` | 検査を起動するキーの変更（SHIFT / CTRL / OPT1 / OPT2） |

### exx.X

68060 の データキャッシュ・命令キャッシュ・ストアバッファ・分岐キャッシュ・スーパースケーラ の ON/OFF 切替。
ストアバッファ・分岐キャッシュ・スーパースケーラは 060turbo.sys が起動時に全て ON にします。
データ/命令キャッシュは純正 CACHE.X と同じ効果。

スイッチ指定: `exx -dc0 -ic0` のように、機能スイッチに 0（OFF）/ 1（ON）を続けます。

| スイッチ | 機能 |
|---------|------|
| `-dc` | データキャッシュ |
| `-ic` | 命令キャッシュ |
| `-sb` | ストアバッファ |
| `-bc` | 分岐キャッシュ |
| `-ss` | スーパースケーラ |

ファイル名指定: exx.x を `edc.x` / `eic.x` / `esb.x` / `ebc.x` / `ess.x`（先頭の `e` は省略可）の名前でコピーすると、
`edc 0` のようにパラメータ 0/1 だけで該当機能を切り替えられます。

68000 用に最適化されたプログラムでは、オペランドワードで分岐命令を飛び越える書き方により分岐予測エラー
（Branch Prediction Error、アクセスエラーの一種）が起き、白帯でバスエラー停止することがあります。「68030 モードでは動くが 68060 モードで
キャッシュを全て OFF にしても止まる」場合は `exx -bc0` を試す（付属 bpe.doc 参照）。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/hardware/060turbo/ （X68000 LIBRARY）。各ツールのサブページに LZH があります。
本ページ記載の版のアーカイブは `arc/retropc.net/x68000/software/hardware/060turbo/` に保存済み。
060opmp / 060prnp は 060dmap と同じ 060PP001.LZH に収録。

### インストール（HDSへの導入手順）

エミュレータ停止中に実行します。`060turbo.sys` は別途導入済みのため対象外。展開には `unar` が必要（`brew install unar`）。
アーカイブは `arc/` のキャッシュを使い、無いときだけ配布元から `curl -L --create-dirs -o` で取得します。

```sh
HDS=disk/SYSTEM_HD.HDS
SRC=arc/retropc.net/x68000/software/hardware/060turbo
OUT=$(mktemp -d)
for a in 060cache/060CACHE.LZH 060dmam/060DMAM2.LZH 060dmap/060PP001.LZH 060high/060HIGH.LZH \
         060loadhigh/060lh011.lzh 060mat/060MAT.LZH 060mmu/060MMU.LZH 060pcr5/060PCR5.LZH \
         060prni/060PI001.LZH 060ps/060PS.LZH 060simck/060SC012.LZH exx/EXX100.LZH; do
  unar -q -e SHIFT-JIS -o "$OUT/$(basename $a)" "$SRC/$a"
done

tools-host/hu68disk/hu68disk mkdir $HDS /BIN/060turbo
find "$OUT" -type f \( -name '*.x' -o -name '*.sys' \) | while read f; do
  tools-host/hu68disk/hu68disk copy $HDS "$f" /BIN/060turbo
done
# exx.x のファイル名別名
for n in ebc edc eic esb ess; do
  tools-host/hu68disk/hu68disk copy $HDS "$(find "$OUT" -name exx.x)" /BIN/060turbo/$n.x
done

# 付属ドキュメント
tools-host/hu68disk/hu68disk mkdir $HDS /DOC/060turbo
find "$OUT" -type f -name '*.doc' | while read f; do
  tools-host/hu68disk/hu68disk copy $HDS "$f" /DOC/060turbo
done
```

060turbo.sys の付属文書（060T056B.LZH の `README`、v0.56 の更新履歴のみ）は `/DOC/060turbo/060turbo.doc` として配置してある
（導入済みの本体は v0.61 のため、文書の版とは一致しない）。

PATH は `autoexec.bat` の `PATH` 行に `C:\BIN\060turbo` を追加する（現行: `PATH C:\;C:\SYS;C:\BIN;C:\BIN\060turbo;C:\BASIC2;...`）。
`autoexec.bat` は CRLF・Shift-JIS のため、バイト列置換で編集して `hu68disk copy` で上書きします。

CONFIG.SYS から常駐させる場合は `DEVICE = C:\BIN\060turbo\060dmap.x` のように絶対パスで書く（常駐型の項参照）。
060simck.sys は 060turbo.sys より前に登録します。

### MCP アクション

起動例（いずれも対話なし。引数なしで実行する形の例）:

```
run_command("060mmu")
run_command("060cache")
run_command("060ps")
```

### 検証メモ

- 検証: 2026-10-28 に SYSTEM_HD.HDS の `\BIN\060turbo` へ上記コマンド列で導入し、PATH を追記した（ホスト側の `hu68disk list` で配置確認のみ）。
  配置は `/BIN/060turbo`（実行ファイル）と `/DOC/060turbo`（.doc）。060mmu.X 以外のツールの起動は未検証。
- 060mmu.X: `C:\BIN` から PATH 経由で `060mmu` を実行し、表示を確認した（`local_memory = 128` のプロファイル）。
  `$10000000-$17FFFFFF` が恒等変換・User・Write-through で表示され、128MiB のローカルメモリと一致します。
  `$00F00000-$00FFFFFF` は Supervisor・Write-through・Write-protected の恒等変換。
  リファレンスの出力例（コピーバック、`$00F00000` が `$11F00000` へ変換、`$10000000-$103FFFFF` が Non-serialized）とは異なります。
  ix68k の 060turbo.sys 設定・実装の差とみられるが、原因は未切り分け。
- 060turbo.sys の起動、ローカルメモリ（`_HIMEM`）、060loadhigh の動作確認は未実施。
