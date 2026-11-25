---
exe: HLK.R
種別: freeware
作者: 立花えり子 / TcbnErik（原作: そると / SALT「X68k SILK Hi-Speed Linker v3.01」）
版: HLK evolution v3.01+15 (2022-04-20)
出典: 添付hlkev.txt/kaiseki.txt。配布元 http://retropc.net/x68000/software/develop/lk/hlkev/
   （配布名は"hlk.r"のまま、リネームせず導入）
判別: -（配布アーカイブ hlkev15.zip 収録版のみ確認）
検証状態: ✅ 起動・リンク動作確認済み（オプション個別検証は未着手）
最終更新: 2026-08-19
---
# HLK.R (hlk)

> 高速リンカ。原作HLK v3.01（そると氏作）の.quad未対応等の不具合を修正し、大容量メモリ対応（060turbo）・オプション追加（`-l<lib>`/`-L`/`-b`/`-g`/`-r`/`-rn`等）を行った改良版。LK.Xの代わりに利用可能とされます。

---

## リファレンス（一次資料要約）

### 起動・コマンドラインオプション

書式（`hlkev.txt`の`-p`オプション説明中の使用例より）:

```
hlk <objfile...> [-オプション...]
```

`objfile`を省略すると出力の拡張子は既定で`.x`になる（`-r`/`-rn`指定時は`.r`）。原作
v3.01の`update.doc`（`\DOC\HLK`収録）は変更履歴の中でオプションに触れているだけで、
正式な書式・全オプションを列挙した文書（SYNOPSIS相当）は入手できていない
（原作の完全なマニュアルは配布アーカイブ群に含まれていない模様）。

原作v3.01由来の基本オプション（`update.doc`の変更履歴中の記述から）:

| オプション | 効果 |
|-----------|------|
| `-o<ファイル名>` | 出力ファイル名を指定する |
| `-l` | オブジェクトやライブラリが見つからない場合、環境変数`lib`が指すパスから探す |
| `-p[ファイル名]` | マップファイルを出力する（省略時は実行ファイル名の拡張子を`.map`に変える） |
| `-w` | コモンラベルの重複定義など、拡張機能に由来する警告を抑制する |
| `-a` | 実行ファイルに`.x`拡張子を付けず、代わりに第7ビットを立てる |
| `-s` | 外部ラベル`___rsize`（絶対値、rdata〜rstack間のサイズ）を定義する |
| `-d` | HLK evolution版の`-d`とは別の、原作固有のオプション（詳細不明） |
| `-m` | 外部定義ラベルの最大個数を指定する（v2.20以降は値の範囲チェックのみで実質無効） |
| `-v` | 詳細情報を表示する（v1.08以降は役割の一部を`-p`に譲っている） |
| `-e` | デフォルトのアライメント値を設定する（v3.01時点の上限は256） |

環境変数`SILK`（未定義なら`silk`）の内容がコマンドライン末尾に追加されます。オブジェクト
ファイル名の先頭に`+`を付けると、そのファイルを先頭リンク扱いにできる（複数指定時は
指定順が保持される）。

HLK evolutionでの追加・変更オプション（`hlkev.txt`参照）:

| オプション | 効果 |
|-----------|------|
| `-h`, `-?`, `--help` | 使い方を表示する |
| `--version` | バージョンを表示して終了する |
| `--verbose` | 詳細表示モードにする（`-v`と同等） |
| `--quiet` | 詳細表示モードを解除する（`-z`と同等） |
| `-an` | 拡張子を自動補完した`-o`指定時、実行属性を自動判定する（拡張子なし指定時のみ実行属性を立てる） |
| `-b <num>` | 実行形式のベースアドレスを指定する（例: `-b0x6800`） |
| `-g <num>` | ロード方式を指定する（0=最大ブロック（既定）/1=最小ブロック/2=高位メモリ。`.r`形式出力時は無視される） |
| `-r` | ヘッダなしの`.r`形式で出力する |
| `-rn` | `.r`形式で出力するが、変換エラー（再配置テーブル使用等）を無視して強制的に作成する |
| `-p[ファイル名]` | マップファイルを出力する（`-p`の直後に空白を空けず指定。省略時は実行ファイル名の拡張子を`.map`にする） |
| `-l<lib>` | `lib<lib>.a`をリンクする（`-l`と`<lib>`の間は空けない） |
| `-L <path>` | ライブラリの検索パスを追加する |
| `-0` | `.ctor`/`.dtor`への対応を無効化する |
| `-1` | `.ctor`/`.dtor`に対応する（gcc2付属の`g2as`/`g2lk`とは非互換な暫定機能） |
| `-d` | シンボル値を常に16進数として解釈する（`0x`/`$`の付与可）。同名・同値の定数（絶対値）が重複定義されてもエラーにしない |
| `-m` | v3.01+15で機能が削除された（指定しても無視されます。将来別機能に割り当てられる可能性あり） |

環境変数`HLK`が参照される（`SILK`/`silk`は見ない）。数値を取るオプション（`-d`を除く）は、
先頭に`0x`または`$`を付けることで16進数として解釈させられます。エラー終了時の終了コードは1。

---

## ix68kでの利用

### 入手先

http://retropc.net/x68000/software/develop/lk/hlkev/ から`hlkev15.zip`としてダウンロード可能（v3.01+15）。
**`arc/hlkev15.zip`にローカル保存済み**（2026-08-12取得）。再検証・再インストール時は
retropc.netへ再度アクセスせず、まずこちらを使うこと。

ソース(`src.zip`)は未取得（本体実行のみに必要なバイナリのみ導入）。

### インストール（SYSTEM_HD.HDSへの導入手順）

CONFIG.SYS登録不要。PATHの通ったディレクトリ（`\BIN`等）に本体を置くだけで使えます。
配布名`hlk.r`のままリネームせず導入（既存の`LK.X`/`HLK.X`は本HDSに未導入のため名前衝突なし）。

```sh
# 1. ダウンロード（arc/hlkev15.zipにローカル保存済みならそれを使い、再ダウンロードしない）
cp arc/hlkev15.zip ./hlkev15.zip
# 無ければ: curl -LO http://retropc.net/x68000/software/develop/lk/hlkev/hlkev15.zip

# 2. 展開（unar、要 brew install unar。ZIP形式）
unar -o ./hlkev_extracted hlkev15.zip
# → ./hlkev_extracted/hlkev15/{src.zip, hlk.r, hlkev.txt, kaiseki.txt}

# 3. 本体(hlk.r)をHDSの\BINへコピー（hu68disk、エミュレータ停止中に実行）
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./hlkev_extracted/hlkev15/hlk.r /BIN

# 4. ドキュメントはHDSの\DOC\HLKへコピー（ix68k上で参照用）
tools-host/hu68disk/hu68disk mkdir disk/SYSTEM_HD.HDS /DOC/HLK
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./hlkev_extracted/hlkev15/hlkev.txt /DOC/HLK
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./hlkev_extracted/hlkev15/kaiseki.txt /DOC/HLK
```

`\DOC\HLK`配下（2026-08-12導入）:`hlkev.txt`（改変履歴・追加オプション説明）・
`kaiseki.txt`（オブジェクトファイル形式の個人解析資料、2022年版=HLK evolution同梱の
最新版を採用）。`src.zip`（ソース）は実行に不要のためHDSへはコピーしません。

原作hlk.x v3.01（http://retropc.net/x68000/software/develop/lk/hlk/ 、
`HLK301B.LZH`）から**ドキュメントのみ**追加導入済み（バイナリ`hlk301.x`は未導入、
実運用はHLK evolution版`hlk.r`を使用）:

```sh
cp arc/HLK301B.LZH ./HLK301B.LZH
# 無ければ: curl -LO http://retropc.net/x68000/software/develop/lk/hlk/HLK301B.LZH
unar -o ./hlk301_extracted HLK301B.LZH
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./hlk301_extracted/HLK301B/readme.doc /DOC/HLK
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS ./hlk301_extracted/HLK301B/update.doc /DOC/HLK
```

`readme.doc`（配布規定・謝辞）・`update.doc`（v1.07〜v3.01の変更履歴、オプション
`-l`/`-p`/`-w`/`-a`/`-s`/`-d`/`-m`/`-v`等の追加・変更経緯を含む）を追加。
同梱の`kaiseki.txt`（1992年版、14.7KB）はHLK evolution同梱の2022年改訂版（16.2KB、
既に導入済み）と同名で内容も更新されているため、重複を避け未導入（旧版）。
`hlk301.x`（バイナリ）はユーザー指示によりドキュメントのみの導入としたため未導入。

### MCP アクション（実行例）

```
run_command("HLK MAIN.O SUB1.O SUB2.O")     # 複数オブジェクトをリンク（.x出力）
run_command("HLK -r -o OUT.R MAIN.O")       # .r形式で出力
```

`\BIN`がPATH外の場合はフルパス指定（`C:\BIN\HLK ...`）で呼び出します。

### 動作確認の再現手順（HAS+HLK連携テスト）

[[HAS.X]]でのアセンブルとセットの手順。テストソース・再現コマンドは
`freeware/HAS.X.md`の「動作確認の再現手順」を参照（HLK単体では入力の`.o`が
作れないため、必ずHAS側の手順から始める）。

### 検証メモ

- 2026-08-12: `arc/hlkev15.zip`から`hlk.r`をHDS(`\BIN`)へ導入済み。
- 2026-08-12: ix68kで起動・リンク動作を確認。`\WORK\HASTEST\HELLO.O`
  （[[HAS.X]]でアセンブル済み）を`HLK HELLO.O`でリンクし`HELLO.X`（98バイト）を生成、
  実行してDOSコール`_PRINT`による文字列表示が正常動作することを確認（オプション無指定）。
- 2026-08-12: `arc/HLK301B.LZH`（原作hlk.x v3.01）から`readme.doc`/`update.doc`のみ
  `\DOC\HLK`へ追加導入（ユーザー指示によりドキュメントのみ、バイナリ`hlk301.x`は未導入）。
- 出典アーカイブ: `hlkev15.zip`（76.9KB、`src.zip`/`hlk.r`/`hlkev.txt`/`kaiseki.txt`を同梱）、
  `HLK301B.LZH`（21.7KB、`readme.doc`/`update.doc`/`kaiseki.txt`/`hlk301.x`を同梱）。
- 未検証: 全オプション、基本のオブジェクト指定・出力動作（原作v3.01の正式な書式・
  全オプション一覧を記した文書は未入手。`update.doc`は変更履歴形式のため断片的）。
