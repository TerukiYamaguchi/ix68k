---
exe: LZZ.R
種別: freeware
作者: NOVA さん
版: v0.57a（`LZZ057.LZH`収録のv0.57に`LZZ057A.LZH`収録の`lzz057_057a.bfd`パッチを
  [BUP.X](BUP.X.md)で適用。19700 bytes、95-02-04付）
出典: 配布元URL不明（リポジトリ内アーカイブコレクション由来）
判別: 同ディレクトリに`LZZ053.LZH`（v0.53、旧版）も保存済み。[MZP.X](MZP.X.md)の
  マニュアル出典`MZP04.LZH`にも`LZZ054.Lzh`（v0.54）が同梱されているが、今回は
  単体配布の`LZZ057.LZH`+`LZZ057A.LZH`（最新版）を使用した
検証状態: ✅ インストール・BUP.Xパッチ適用・バージョンバナー確認済み（`/BIN`で
  `LZZ`実行し`v0.57a`表示を確認）。ZDFファイルの実展開動作も[MMDSP.R](MMDSP.R.md)経由で
  確認済み。MDFの展開・圧縮、単体コマンドとしての`LZZ -E/-C`実行は未検証
最終更新: 2026-09-03
---
# LZZ.R (LZZ)

> ミュージックデータ（MDF/ZDF形式）の展開・圧縮ツール。RCシステムの`LZM`をアセンブラレベルで
> 小型化・高速化し、リロケータブル形式へ書き換えたもの。[MZP.X](MZP.X.md)や各種RCP系
> プレイヤがMDF/ZDF形式の演奏データを扱う際に内部から呼び出す（ファンクション利用）ほか、
> 単体コマンドとしても実行できます。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式:

```
LZZ [-option...] <File Name> [<File Name> ...]
```

複数ファイル指定・ワイルドカードに対応。主なオプション（大文字小文字を区別する）:

| オプション | 効果 |
|-----------|------|
| `E` | MDF/ZDFファイルを展開する |
| `C` | ファイルを圧縮する（ZMD/MDX/MDN/MDZ/ZMS/OPM/MDD等に対応） |
| `L` | 圧縮ファイルの内容をリスト表示する |
| `R` | ファイル名を変更する（`lzz -r <旧名> <新名>`） |
| `H` | メモリを多く消費する代わりに高速に圧縮する（既定） |
| `S` | メモリ消費が少ないぶん圧縮が遅い |
| `D` | LZD形式で圧縮する（`H`/`S`は無効になる） |
| `F` | 環境変数`lzz_dest`が設定されていても強制的にカレントへ作成する |
| `I` | PCMファイルを圧縮しない（複数データから共用するPCMファイル向け） |
| `M` | MDDファイルを圧縮しない（複数データから共用するMDDファイル向け） |
| `B`/`P`/`K`/`W` | バッファ上で展開後に`RCP -B`/`-P`/`-K`/`-W`を起動する（`B`が既定） |
| `X` | バッファ上で展開のみ行う |
| `N` | コメントを表示しない |

環境変数`lzz_opt`（1文字ずつ連結）で既定動作を変更でき、`C`=ドライブ入れ替えチェック、
`P`=TwentyOne互換の拡張子付与、`S`=無圧縮回避、`Z`=MDIをZDFに圧縮、`p`=PCMを無圧縮格納が
指定できます。`lzz_conv_opt`はRCコンバータ呼び出し時の追加オプション、`lzz_dest`は
ファイル作成先ディレクトリを指定します。255文字を超える引数は`lzz -+-+-<file>`形式の
インダイレクトファイルで渡せます。

### HUPAIR対応について

付属の差分（`lzz_hupair.bfd`）を当てると`lzz.x`（HUPAIR対応版）が生成できます。`lzz.x`を
使う場合でも`lzz.r`は別途必要（MZP rel.4a以降は`lzz.x`単体でも可）。本プロジェクトでは
HUPAIR対応は未導入（`lzz.r`のみインストール）。

### アプリケーションからの利用（ファンクション）

他プログラムからDOSコール`EXEC`(MD=3)で`lzz.r`をサブルーチンとして読み込み、
`LZZADR+12`（`ref_data`）で展開情報取得、`LZZADR+16`（`ext_data`）で実展開を行う
2段階のAPIを持ちます。詳細は同梱`LZZ.DOC`参照。[MZP.X](MZP.X.md)はこの経路でLZZを利用します。

---

## ix68kでの利用

### 入手先

`arc/x68tools/SOUND/ZMUSIC/MZP/`に複数版を保存済み（配布元URL不明、リポジトリ内
アーカイブコレクション由来）:

- `LZZ057.LZH` — v0.57本体（`LZZ.DOC` `LZZ_UP.DOC` `lzz.r` `LzzFunc.Lzh` `lzz_hupair.bfd`）
- `LZZ057A.LZH` — v0.57→v0.57aのバージョンアップパッチ（`lzz057_057a.bfd`、
  [BUP.X](BUP.X.md)形式の差分。実体ファイルは含まない）
- `LZZ053.LZH` — v0.53（旧版、未使用）

**再検証・再インストール時は上記ローカルアーカイブを使うこと**（配布元不明のため
再ダウンロード不可）。

### インストール（SYSTEM_HD.HDSへの導入手順）

```sh
# 1. 展開（unar、要 brew install unar）
unar -o ./lzz057_extracted arc/x68tools/SOUND/ZMUSIC/MZP/LZZ057.LZH
unar -o ./lzz057a_extracted arc/x68tools/SOUND/ZMUSIC/MZP/LZZ057A.LZH
# → ./lzz057_extracted/LZZ057/lzz.r （v0.57本体）
# → ./lzz057a_extracted/LZZ057A/lzz057_057a.bfd （v0.57a差分）

# 2. HDS内に作業ディレクトリを作り、v0.57本体と差分を配置（エミュレータ停止中）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /TMP
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./lzz057_extracted/LZZ057/lzz.r /TMP
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./lzz057a_extracted/LZZ057A/lzz057_057a.bfd /TMP
```

```
# 3. エミュレータ起動後、ix68k内で（MCP run_command等。BUP.Xは/BINに導入済み）
CD \TMP
BUP lzz057_057a.bfd -NT
# → `lzz.r' ... completed.（更新前ファイルは \TMP\orig へ自動バックアップ）
COPY lzz.r C:\BIN\lzz.r
```

```sh
# 4. 作業ディレクトリを削除（ホスト側、hu68disk）
tools-host/hu68disk/hu68disk del disk/SYSTEM_HD.HDS /TMP/orig/lzz.r
tools-host/hu68disk/hu68disk rmdir disk/SYSTEM_HD.HDS /TMP/orig
tools-host/hu68disk/hu68disk del disk/SYSTEM_HD.HDS /TMP/lzz.r
tools-host/hu68disk/hu68disk del disk/SYSTEM_HD.HDS /TMP/lzz057_057a.bfd
tools-host/hu68disk/hu68disk rmdir disk/SYSTEM_HD.HDS /TMP
```

`lzz.r`は`LZZ057A.LZH`単体には実体を含まない（`.bfd`差分のみ）ため、必ずv0.57本体
（`LZZ057.LZH`）を先にインストールしてからパッチを当てます。BUP.Xの詳細（タイムスタンプ
チェックの`-NT`が必要な理由等）は[BUP.X.md](BUP.X.md)参照。

### 検証メモ

- 検証: 2026-09-03、上記手順で`/BIN/lzz.r`へインストール。`BUP lzz057_057a.bfd -NT`が
  `` `lzz.r' ... completed.``で正常終了し、`/BIN`で`LZZ`を実行して起動バナーが
  `X68k LZZ:  Music data Compress / Extract v0.57a (c)1992-95 NOVA`と表示されることを
  確認しました。
- 2026-09-03、ユーザーが手元のZDFファイルを[MMDSP.R](MMDSP.R.md)で演奏し、正常に再生
  できることを確認（内部でLZZ.RによるZDF展開が行われている）。
- MDF実データを用いた展開・圧縮そのものの動作確認、単体コマンドとしての`LZZ -E/-C`直接実行、
  および[MZP.X](MZP.X.md)からの呼び出し確認は未実施。
