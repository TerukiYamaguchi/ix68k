---
exe: BUP.X
種別: freeware
作者: 東出浩一さん
版: v1.2 Rel.2（BdifSet.Lzh収録）
出典: 配布元 http://retropc.net/x68000/software/tools/archiver/bdifset/ （X68000 LIBRARY）
判別: -（BdifSet.Lzh収録版のみ確認。BdifSetにはbdif.x/bfd2rx.x/revdel.xも同梱だが未インストール）
検証状態: ✅ インストール・パッチ適用動作確認済み（[ZMUSIC.X](ZMUSIC.X.md) 2.08→2.08e適用で使用）
最終更新: 2026-08-19
---
# BUP.X (bup)

> バイナリ差分更新プログラム。`bdif.x`が作成した差分ファイル（拡張子`.bfd`）を使い、古いバージョンのファイルを新しいバージョンへパッチ更新します。ZMUSIC 2.08→2.08e等の差分配布パッチ適用に使用。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式:

```
BUP <差分ファイル名>[.bfd] [オプション]
```

差分ファイル名の拡張子`.bfd`は省略できます。既定ではカレントディレクトリにある更新対象と
同名のファイルを、差分情報に従って書き換える（`-I`/`-O`で入出力ディレクトリを分離可能）。
出典は同梱`doc/bup.doc`（v1.2 Rel.1、1992-12-28付）。

| オプション | 効果 |
|-----------|------|
| `-R` | 逆方向のパッチを当てる（差分ファイルに逆方向の情報が記録されている場合のみ有効） |
| `-L` | 差分ファイルに記録されている差分内容を表示するだけで、実際の更新は行わない |
| `-LM` | `-L`と同じだが、表示するファイル名を12文字までに切り詰める |
| `-E` | 差分ファイル名としてLZHファイルの拡張子を指定した場合に、差分以外の中身をそのまま展開する |
| `-C` | ファイルの整合性チェックのみ行い、実際の更新はしない |
| `-I <dir>` | 更新対象ファイルを探すディレクトリを指定する（省略時はカレントディレクトリ） |
| `-O <dir>` | 更新結果を書き出すディレクトリを指定する（省略時はカレントディレクトリ） |
| `-NT` / `-NC` / `-NL` / `-NA` | 更新前チェックで見るタイムスタンプ／CRC／ファイルサイズ／属性のいずれかを個別に無視する（`-NC`と`-NL`は原文で非推奨と明記されている） |

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/tools/archiver/bdifset/ から`BdifSet.Lzh`をダウンロード可能
（`bdif.x` `bup.x` `bfd2rx.x` `revdel.x`一式同梱）。

**`arc/BdifSet.Lzh`にローカル保存済み**（2026-08-06取得）。再検証・再インストール時は
retropc.netへ再度アクセスせず、まずこちらを使うこと。

### インストール（SYSTEM_HD.HDSへの導入手順）

```sh
# 1. ダウンロード（arc/BdifSet.Lzhにローカル保存済みならそれを使う）
cp arc/BdifSet.Lzh ./BdifSet.Lzh
# 無ければ: curl -LO http://retropc.net/x68000/software/tools/archiver/bdifset/BdifSet.Lzh

# 2. 展開（unar、要 brew install unar）
unar -o ./bdifset_extracted BdifSet.Lzh
# → ./bdifset_extracted/BdifSet/bin/{bdif.x, bup.x, bfd2rx.x, revdel.x}

# 3. bup.xをHDSの\BINへコピー（hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./bdifset_extracted/BdifSet/bin/bup.x /BIN
```

### 検証メモ

- 検証: 2026-08-06、ix68kで`ZMUSIC.X`のver2.08→2.08eパッチ適用に使用。
  - `BUP ZMUSIC.BFD`（オプションなし）→ hu68diskでホストからコピーした対象ファイルの
    タイムスタンプが配布時オリジナルと異なるため`Error: タイムスタンプが違います`で拒否。
  - `BUP ZMUSIC.BFD -NT`（タイムスタンプチェック無視）→ 正常完了
    （`` `ZMUSIC.X' ... completed.``）。更新前ファイルはカレントディレクトリの
    `orig`サブディレクトリへ自動バックアップされることを確認。
  - パッチ後リブートでZMUSIC起動バナーが「2.08e」表示に変化したことで、
    パッチ結果の正当性を確認済み。
  - **結論: インストール・パッチ適用動作とも正常確認済み。**
