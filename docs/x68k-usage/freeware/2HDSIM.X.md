---
exe: 2HDSIM.x
種別: freeware
作者: TAKE さん
版: v1.1（`2HDSIM10.LZH`収録のv1.0に`2HDSIM11.LZH`収録の`2HDSIM11.BFD`パッチを
  [BUP.X](BUP.X.md)で適用。4622 bytes、94-06-17付）
出典: http://retropc.net/x68000/software/disk/floppy/2hdsim/ （X68000 LIBRARY）
判別: -
検証状態: ✅ インストール・BUP.Xパッチ適用・バージョンバナー確認済み（`2HDSIM -?`で
  `Ver.1.1`表示を確認）。イメージファイル登録・アクセス・EJECT.xによる切替の実動作は未検証
最終更新: 2026-09-03
---
# 2HDSIM.x (2HDSIM)

> 2HDフロッピーディスクの丸ごとバックアップイメージファイルを、仮想的なフロッピー
> ディスクドライブとして常駐アクセスできるようにするドライバ。付属の[MKIMG.x](#mkimgx)
> でイメージファイルを作成し、[EJECT.x](#ejectx)で登録済みイメージを巡回切替します。

---

## リファレンス（一次資料要約）

### 2HDSIM.x — 起動・コマンドラインオプション

書式:

```
2HDSIM.x  [<スイッチ>]  [<ドライブ番号:[-]>]  [<イメージファイル名>]
```

| 引数 | 説明 |
|-----|------|
| スイッチ `-R` | 常駐部を解放し、イメージファイルへのアクセスを無条件に無効化する |
| ドライブ番号 | `0`〜`3`。Human68kの論理ドライブ名（A:〜）とは別体系で、FDDの物理番号を指す |
| イメージファイル名 | [MKIMG.x](#mkimgx)で作成した2HDイメージファイルを指定 |

スイッチ・ドライブ番号・イメージファイル名をすべて省略、または`-?`/`-h`指定で使い方を
表示する（未常駐時のみ。常駐済みなら現在の登録状態を表示する）。`-`の代わりに`/`も使えます。

ドライブ番号の直後に`0:-`のように`-`を付けると、そのドライブの現在アクティブな
イメージファイル登録を1件削除する（次に登録されているファイルへ自動的に切り替わる）。

1つのドライブに複数のイメージファイルを登録できる（登録上限は合計50ファイル、
1ファイルあたり常駐部32バイト消費）。同一ドライブへの複数登録は登録順の巡回リストになり、
[EJECT.x](#ejectx)発行のたびに次のファイルへ切り替わる。最初の登録時に自動的に常駐します。

イメージファイルがリードオンリー属性の場合、アクセスもリードオンリーになる
（ライトプロテクトと同等）。

### 2HDSIM.x — 動作条件・注意

- アクセス対象はMFM倍密モード・セクター長3（1024バイト）・シリンダ`0`〜`$4C`・
  セクター`1`〜`8`の2HD規格のみ。それ以外は素通りして実FDDへのアクセスになります。
- IOCS（`$41` `$45` `$46` `$4D` `$4E` `$4F`）にフックしてFDDアクセスを検出し、
  実際のイメージ読み書きはDOSコール`_DISKRED`/`_DISKWRT`で行います。
- **登録後はDRIVE.X等で論理ドライブの入れ替えを行ってはいけない**（検出できないため
  誤ったファイルへアクセスし内容破壊の恐れがある）。
- **登録済みのバックアップファイル自体をHuman68k側のファイル操作で触ってはいけない**
  （登録後はHumanの管理外でアクセスされるため）。
- FDDに対するディスクキャッシュとの併用は非推奨（多重呼び出し非対応のキャッシュソフトで
  誤動作しうる）。
- 登録するイメージファイルは連続クラスタに存在する必要がある（登録時にチェックされます。
  引っかかった場合はデフラグツールで再配置する）。
- IOCSを介さない独自ルーチンでFDDアクセスするソフト（例: 9 SECTOR DISK DRIVER）とは併用不可。

### MKIMG.x — 起動・コマンドラインオプション

書式:

```
MKIMG.x  [<スイッチ>]  [<ドライブ:>]  <イメージファイル名>
```

| スイッチ | 効果 |
|---------|------|
| `-B`（既定・省略可） | 指定ドライブの内容を丸ごとバックアップしてイメージファイルを作成する |
| `-R` | 指定のイメージファイルを指定ドライブへリストアする（確認のキー入力待ちあり。リストア先には事前にフォーマット済み2HDディスクが必要） |
| `-E` | ディスクなしで空きディスク相当のイメージファイルを新規作成する |

ドライブはHuman68kの論理ドライブ名（A:〜Z:）で指定し、そのドライブが2HD
（互換ラムドライブ含む）である場合のみ動作します。イメージファイル名がすでに存在する場合、
作成は中断されます。実装は`_DISKRED`/`_DISKWRT`と2HD判定用のDPB参照のみで、IOCSは使わない
（FDD互換ラムドライブにも対応するため）。

### EJECT.x — 起動・コマンドラインオプション

書式:

```
EJECT.x  [<ドライブ:>]
```

指定ドライブ（省略時はカレントドライブ）に対してDOSコール`_DRVCTRL`でイジェクトを発行します。
リムーバブルメディアならイジェクト、SASI-HDならシッピング、SCSI-HDなら何も起きない
（環境依存）。2HDSIM.xに複数登録したイメージファイルの切替に使います。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/disk/floppy/2hdsim/ から3種のLZHを配布（2023-11-12更新）:

- `2HDSIM10.LZH` (11.2KB) — v1.0本体。`2HDSIM.DOC` `2HDSIM.x` `MKIMG.DOC` `MKIMG.x`
  `EJECT.DOC` `EJECT.x`
- `2HDSIM11.LZH` (343B) — v1.0→v1.1のバージョンアップパッチ。`2HDSIM11.DOC`
  `2HDSIM11.BFD`（[BUP.X](BUP.X.md)形式の差分、実体ファイルは含まない）
- `2HDSIM11S.LZH` (5.5KB) — v1.1のソースコード（未使用）

**`arc/retropc.net/x68000/software/disk/floppy/2hdsim/`にローカル保存済み**
（2026-09-03取得）。再検証・再インストール時は配布元へ再アクセスせずこちらを使うこと。

### インストール（SYSTEM_HD.HDSへの導入手順）

```sh
# 1. 展開（unar、要 brew install unar）
unar -o ./2hdsim10_extracted arc/retropc.net/x68000/software/disk/floppy/2hdsim/2HDSIM10.LZH
unar -o ./2hdsim11_extracted arc/retropc.net/x68000/software/disk/floppy/2hdsim/2HDSIM11.LZH
# → ./2hdsim10_extracted/2HDSIM10/{2HDSIM.x, MKIMG.x, EJECT.x, *.DOC}（v1.0本体）
# → ./2hdsim11_extracted/2HDSIM11/2HDSIM11.BFD（v1.0→v1.1差分）

# 2. パッチ不要な2ファイルは直接\BINへ、パッチ対象は\TMPへ配置（エミュレータ停止中）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./2hdsim10_extracted/2HDSIM10/MKIMG.x /BIN
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./2hdsim10_extracted/2HDSIM10/EJECT.x /BIN
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /TMP
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./2hdsim10_extracted/2HDSIM10/2HDSIM.x /TMP
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./2hdsim11_extracted/2HDSIM11/2HDSIM11.BFD /TMP

# 付属ドキュメント
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/2HDSIM
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./2hdsim10_extracted/2HDSIM10/2HDSIM.DOC /DOC/2HDSIM
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./2hdsim10_extracted/2HDSIM10/MKIMG.DOC /DOC/2HDSIM
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./2hdsim10_extracted/2HDSIM10/EJECT.DOC /DOC/2HDSIM
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./2hdsim11_extracted/2HDSIM11/2HDSIM11.DOC /DOC/2HDSIM
```

```
# 3. エミュレータ起動後、ix68k内で（MCP run_command等。BUP.Xは/BINに導入済み）
CD \TMP
BUP 2HDSIM11.BFD -NT
# → `2HDSIM.x' ... completed.（更新前ファイルは \TMP\orig へ自動バックアップ）
COPY 2HDSIM.x C:\BIN\2HDSIM.x
```

```sh
# 4. 作業ディレクトリを削除（ホスト側、hu68disk）
tools-host/hu68disk/hu68disk del disk/SYSTEM_HD.HDS /TMP/orig/2HDSIM.x
tools-host/hu68disk/hu68disk rmdir disk/SYSTEM_HD.HDS /TMP/orig
tools-host/hu68disk/hu68disk del disk/SYSTEM_HD.HDS /TMP/2HDSIM.x
tools-host/hu68disk/hu68disk del disk/SYSTEM_HD.HDS /TMP/2HDSIM11.BFD
tools-host/hu68disk/hu68disk rmdir disk/SYSTEM_HD.HDS /TMP
```

v1.1パッチは2HDSIM.x本体のみが対象（IOCS `$4E`が動作していなかったバグ修正）。
MKIMG.x/EJECT.xはv1.0のまま変更なし。`2HDSIM11.BFD`単体には実体ファイルを含まない
（差分のみ）ため、必ずv1.0本体（`2HDSIM10.LZH`）を先にインストールしてからパッチを当てます。
BUP.Xの`-NT`が必要な理由等は[BUP.X.md](BUP.X.md)参照。

### 検証メモ

- 検証: 2026-09-03、上記手順で`/BIN/2HDSIM.x` `/BIN/MKIMG.x` `/BIN/EJECT.x`へインストール。
  `BUP 2HDSIM11.BFD -NT`が`` `2HDSIM.x' ... completed.``で正常終了。
- `2HDSIM -?`実行でバージョンバナーが`2HD Disk Simulator Ver.1.1  Copyright 1994 by TAKE`と
  表示され、パッチ適用を確認しました。
- `MKIMG`実行で`2HD Disk Backup & Restore Ver.1.0  Copyright 1994 by TAKE`のヘルプ表示を確認。
- `EJECT ?`実行で`Drive ejecter Ver0.00`のバナーと「ドライブ指定が間違っています」の
  エラーメッセージを確認（無効なドライブ指定に対する正常なエラー応答）。
- イメージファイルの実登録・アクセス・EJECT.xによる巡回切替の実動作は未検証
  （`MKIMG -E`での空きイメージ作成→`2HDSIM 0: <path>`での登録→`FORMAT`/`DIR`での
  アクセス確認が今後の検証候補）。
