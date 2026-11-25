# TEXT PROBE profiles

`text_probe(action="enable", profile="<name>", binary_path="...")` は、ここにある
`<name>.toml` を読み、対象ファイルの SHA-256 と各入口の実行時バイト署名が一致した場合だけ
定義を ix68k へ投入します。

固定アドレスなら `pc` を使います。シンボル表を持つ再配置可能な**非圧縮** `.X` は `symbol` を使います。
Python側がHuman68k X形式末尾のシンボル表を読み、TEXTシンボルの値から `file_offset` を導出します。
シンボル表がない場合は `file_offset`（64 byteのHuman68kヘッダを含むファイル先頭からの位置）を
指定します。いずれも `[target]` の `scan_start` / `scan_end` で入口署名を一意に探索してロードベースを
求め、実行時PCを導出します。自己展開形式は展開後コードがファイルに対応しないため、この方式を使えません。
`example.toml` は書式だけを示すテンプレートで、実在バイナリには一致しません。`otoko-rebuilt` と
`netbsd-x68k-10-1` の実プロファイルは、実行時PC・入口署名・ABIを確定済み（Phase 5）。

NetBSDカーネルのように対象ファイルがゲスト内のディスクイメージにしか存在しない場合は、
`tools-host/netbsd/ffs_cat.py` でイメージから取り出し、そのファイルを `binary_path` に渡す
（手順は「NetBSD/x68k 10.1（`liveimage-x68k-with-packages-20250106`）での確認例」節）。
プロファイル名は `[a-z0-9][a-z0-9_-]*` で、ドットは使えません。

## プローブ作成手順

1. **対象ファイルを固定します。** `sha256` を採取し、圧縮形式の識別子も確認します。非圧縮X形式に
   シンボル表があれば、関数名を `symbol` として記録します。自己展開形式ではファイルオフセットを
   実行時PCとして使いません。
2. **ゲスト側で正しい作業ディレクトリから起動します。** 相対パスで画像・データを読むプログラムは、
   実行ファイルのフルパス指定だけでは正しく動かないことがあります。
3. **実行時の入口を確定します。** ソースまたはシンボルから候補を得て、ix68kの `DISASM` / `MEM PEEK`
   で命令列とABIを照合します。入口署名は最低8バイト、可能なら関数固有の先頭命令列を使います。
4. **値の受け渡しをABIどおりに書きます。** 68000 Cのスタック引数は入口時の `A7+4` から始まる。`char`
   を32-bitスタックスロットへ積む処理では、下位1 byteを `CHAR S+4 1` として採取できます。
   カーネルのプローブには `space = "supervisor"` を指定します。入口署名の照合を `MEM PEEK ... SUPER`
   で行い、ゲストがユーザーモード中でも `text_probe enable` が通る（既定の `current` は現行モードの
   アドレス空間で読む）。あわせて、ヒット時もスーパーバイザ実行中だけ採取する（同じ論理アドレスを
   実行するユーザープロセスの誤発火を除く）。`space` を省略したプローブはモードを問いません。
5. **起動ログを分離して検証します。** OS起動後に `TEXT EVENTS` を一度取得してリングを空にしてから
   プローブを有効化し、対象プログラムを起動します。`source` と `probe` を確認して、期待する生バイトが
   取れていることを確認します。

### 非圧縮 Human68k 実行形式のロードベース

Human68k実行形式ではファイル先頭の64 byteがヘッダです。実行中PCの命令列をファイル内で検索し、
その一致位置を `file_offset` とすると、次でロードベースを求められます。

```
load_base = runtime_pc - (file_offset - 0x40)
runtime_pc_of_function = load_base + (function_file_offset - 0x40)
```

この算出後も必ず `MEM PEEK` の入口署名で確認します。`symbol` profile はX末尾の `0x0201`（TEXT）
シンボルを読み、この式の `file_offset` を自動導出します。`file_offset` profile はその値を直接指定します。
メモリ割当てや自己展開によってロードベースが変わる場合、固定PCプロファイルを作成してはなりません。
自己展開形式には展開後コードの入口署名検索またはシンボル解決を追加してから有効化します。

### 男弾（非圧縮再ビルド版）での確認例

`C:\\WORK\\OTOKO` から起動した再ビルド版で、次の入口とABIを確認しました。

| 関数 | symbol / file_offset | 実行時PC（実測時） | 引数 | MCP定義 |
| --- | --- | --- | --- |
| `TxfontPuts` | `_TxfontPuts` / `$18296` | `$180F46` | `char *` = `A7+4` | `STRING S+4 4096 title.otoko TxfontPuts shift-jis` |
| `TxfontPutchar` | `_TxfontPutchar` / `$181EC` | `$180E9C` | `char` の下位 byte = `A7+4` | `CHAR S+4 1 title.otoko TxfontPutchar raw` |
| `TxfontCursor` | `_TxfontCursor` / `$18144` | `$180DF4` | `short x` = `A7+4`, `short y` = `A7+8` | 現行イベント形式では座標更新規則が未実装のため登録しない |

このPCは当該メモリ配置での値であり、プロファイルへ固定値として登録する前に、毎回入口署名を照合します。

### NetBSD/x68k 10.1（`liveimage-x68k-with-packages-20250106`）での確認例

対象: `teokurebsd.org/netbsd/liveimage/20250106-x68k/liveimage-x68k-with-packages-20250106.zip`
展開後の `/netbsd`（3,407,336 bytes、非stripのa.outカーネル、SHA-256は下記）。カーネルは
再配置されない固定ベースで動作するため、`nm /netbsd` が返すシンボルアドレスがそのまま実行時PCになる
（Human68k `.X` の `symbol`/`file_offset` 方式のようなロードベース計算は不要）。

| 関数 | 実行時PC | 引数 | MCP定義 |
| --- | --- | --- | --- |
| `iteputchar` | `$8B04` | `int c` = `A7+4`（第2引数 `struct ite_softc *` = `A7+8`） | `CHAR S+4 1 console.netbsd iteputchar raw` |
| `itecnputc` | `$AECC` | `dev_t dev`（64 bit）= `A7+4`..`A7+11`, `int c` = `A7+12`（0x0C） | `CHAR S+C 1 console.netbsd itecnputc raw` |

`iteputchar` はTTY/ログインセッションの通常出力（ログイン画面、シェルプロンプト、コマンド出力）を
1文字ごとに受け取る、ite(4)ドライバの実体。`itecnputc` はカーネル内部の `printf`/`kprintf`
（コンソール出力フラグ付き）が経由する、`cn_tab->cn_putc` 経由の低レベル出力。両者は異なる経路
（前者はtty回線規則、後者は`cnputc`ディスパッチャ）で同じ描画処理へ収束するが、共通の下請け関数へは
到達しないため、TTY出力とカーネルメッセージの両方を捉えるには両方を登録します。

ABIはブレークポイントで両関数の入口に実際に停止させ、`GET REGISTERS`のスタック（`A7+4`/`A7+8`）と
逆アセンブル結果（`MOVE.L 8(A6),D2` 等、`LINK`/`MOVE.L A6,-(A7)`プロローグ後のオフセット）を突き合わせて
確認した（`disassemble`/`MEM PEEK`はCPUがユーザモードのタイミングで呼ぶとカーネル空間を正しく
訳せない＝空文字列や無関係なバイト列が返ることがあるため、対象アドレスへブレークポイントを張って
実際にヒットさせた状態で読むこと）。

SHA-256（`a431f4be07a6cbd15c374e41011656b9aef94b41849ea27d340608f06e5801fe`）はイメージ内の
`/netbsd` に対するもので、ゲストを起動せずに取り出して照合できます。

```
python3 tools-host/netbsd/ffs_cat.py <liveimage.hds> /netbsd -o <出力先>/netbsd
```

`ffs_cat.py` は X68SCSI1 ヘッダ付きの `.hds`（X68K パーティション表の最初の `BSD ffs`、UFS1・
ビッグエンディアン）から1ファイルを読み取り専用で取り出す（`--partition N` で番号指定）。取り出した
ファイルを `text_probe(action="enable", profile=..., binary_path=<出力先>/netbsd)` へ渡します。
このイメージの `/netbsd` を取り出した結果の SHA-256 は上記と一致することを確認済み。

`TEXT EVENTS` で `\x1b[H`（カーソルホーム）・`\x1b[J`（画面消去）等のANSIエスケープシーケンスが
`encoding="raw"` のまま個別バイトとして保持されることも確認しました。

このライブイメージの `/etc/rc` は無関係な既知の問題を複数抱える（`dmesg`・`fccache`・`motd`の
各rc.dスクリプトが `Segmentation fault` で終了する、`fccache`はフォントキャッシュ構築に非常に
長時間かかる等）。プローブ動作には影響しないため本セッションでは深追いしていません。

### NetBSD/x68k 10.1 ソースビルド版（`netbsd-x68k-10-1-src`）

`build.sh -m x68k kernel=GENERIC` でビルドした未 strip の `netbsd`（ELF）用。ホスト側にファイルがあるため、
`text_probe(action="enable", profile="netbsd-x68k-10-1-src", binary_path="<ビルド出力の netbsd>")` で
SHA-256 照合つきに有効化できます。入口は Live Image 版と同じ `iteputchar`=`$8B04`・`itecnputc`=`$AECC`
（`nm` と入口署名をホスト上のファイルで照合済み、ABI は `objdump -d` の `fp@(8)`/`fp@(12)` で確認）。
カーネルは固定ベース（VirtAddr 0）で、strip してもアドレスは変わりません。ゲストへ入れるのが strip 済みでも
同じプローブが使える（SHA はホスト側の未 strip ファイルに対して照合する）。

- 再ビルドすると日時が埋め込まれ SHA-256 が変わる。再ビルドしたら `[target] sha256` と、`nm` で出した
  アドレス・入口署名を更新します。
- プロファイル名はドットを含められない（`_PROFILE_NAME_RE` が `[a-z0-9][a-z0-9_-]*`）。
- ix68k でのプローブ動作（`TEXT EVENTS` で dmesg が取れること）は未確認です。

### HIOCS.X 1.10+16（Human68k のコンソール出力）

通常は、このプロファイルを手動で有効にする必要はありません。MCP サーバが、DOS コール表の `_PUTCHAR`（`$1808`）から
HIOCS.X・IOCS.X（SHARP 純正）の `_B_PUTC` 本体を実行時に特定し、同じ採取を自動で有効にする
（`tools-host/mcp/MCP_TOOLS.md` の `text_probe()` の「出力採取点の自動有効化」の項）。以下は手動で有効にする場合の手順。

対象: `disk/SYSTEM_HD.HDS` の `/SYS/HIOCS.X`（28,752 バイト、非圧縮 `.X`、SHA-256 は `hiocs.toml` の `[target]`）。
ホスト側へは `tools-host/hu68disk/hu68disk extract <HDS> /SYS/HIOCS.X <出力先>` で取り出し、
そのファイルを `binary_path` に渡します。

```
text_probe(action="enable", profile="hiocs", binary_path=<出力先>/HIOCS.X)
```

HIOCS.X を組み込むと、コマンド出力（`PROCESS`、`ECHO`、`TYPE` など）が `read_stdout_text` で取れません。
HIOCS.X が DOS コールのテーブル（`$1800`）の `_PUTCHAR`（`$FF02`）・`_INPOUT`（`$FF06`）・`_PRINT`（`$FF09`）などを
自前のフックへ差し替え、標準出力がコンソールのとき `_B_PUTC` 本体を IOCS（TRAP #15）を経由せずに直接呼ぶため。
ix68k の出力キャプチャは TRAP #15 のハンドラにしかないので、この経路は通りません。

```
DOS コール（ライン F）→ DOS ディスパッチャ（$1800 のテーブル）→ HIOCS のフック
  → IOCS ベクタ $0480 が $05A124 のままなら BSR $05A14A、違えば MOVEA.L ($0480),A0 / JSR (A0)
  → _B_PUTC 本体 → スクロール・TVRAM 描画
```

| 関数 | HIOCS.X 内のオフセット | 実行時 PC（実測時） | 引数 | プローブ |
| --- | --- | --- | --- | --- |
| `_B_PUTC` 本体 | `0x420` | `$05A14A` | 文字コード = `D1` の下位 1 byte | `CHAR d1 1`（`action = "char"`, `value = "d1"`, `size = 1`） |
| `_PUTCHAR` フック | `0x34D0` 付近 | `$05D1F6`（DOS テーブルの飛び先） | — | 登録しない |
| `_PRINT` フック | `0x3566` 付近 | `$05D28C`（DOS テーブルの飛び先） | — | 登録しない |

`_B_PUTC` 本体の入口は、DOS コール経由の高速経路（`BSR $05A14A`）、`$0480` 経由の `JSR (A0)`、IOCS の
`_B_PUTC`（入口 `$05A124` → 本体）のどれも通るため、1 か所で足りる。ロードベースは入口署名
（`1038099066260cb8`、`MOVE.B $990.W,D0` / `BNE` / `CMPI.L`）を `0x50000`〜`0x70000` で走査して求める
（`file_offset = 0x420`、HIOCS の本体は `$059D6A` から載った構成で確認）。

確認方法（このプロファイルを作る根拠）:

- `trace_iocs` で `ECHO` を実行すると、記録された IOCS は 0x00 / 0x01 / 0x02 / 0x0D / 0xF5 のみで、
  0x20 / 0x21 / 0x2F は 0 件。
- `_B_PUTC` のベクタ先（`$0480` → `$05A124`）と入口の少し先（`$05A142`）にブレークポイントを置いても
  `hits=0`。本体の `$05A14A` に置くと `PROCESS.X` の 1 文字目（`D1=$58`）で止まり、スタックの戻り先が
  `$05D2B6`（`_PRINT` のフック内）と `$0085F6`（DOS ディスパッチャの `JSR (A0)` の直後）になります。
- HIOCS.X のファイル内に、`_B_PUTC` 本体・`_PUTCHAR` フック・`_PRINT` フックのバイト列があります。実行時アドレスとの
  差が 3 つとも同じ。HUMAN.SYS には該当のバイト列がありません。IOCS.X（SHARP 純正）にも同じ構造があるが、バイト列とオフセットは別（下記）。
- `$0480` を `JMP $05A124` だけのスタブへ向けても、飛び先を `JSR (A0)` で直接呼ぶため、TRAP #15 を通らず取れません。

### IOCS.X（SHARP 純正）と、自動有効化が使う署名・オフセット

`\SYS\IOCS.X`（16,034 バイト、SHA-256 `3c88d0dd…f9a396`）も、HIOCS.X と同じ経路でコンソール出力を TRAP #15 から外す
（IOCS.X と HIOCS.X は排他で、どちらか一方だけが常駐する）。DOS `_PUTCHAR` のフックは、標準出力がコンソールのとき
D1 に 1 byte を入れて `MOVEA.L $0480.L,A0 / JMP (A0)` で IOCS の `_B_PUTC` ベクタを直接呼び、そこから `BSR` で共通の本体へ入る。
IOCS の `_B_PRINT` も同じ本体を呼びます。標準出力のハンドルがコンソール（フラグ `1`）でないとき（`> file` のリダイレクト中は `5`）は、
元のハンドラへ進むので、この経路を通りません。

MCP サーバの自動有効化は、DOS コール表の `_PUTCHAR`（`$1808`）が指す先の入口署名でドライバを判別し、
`$1808` の値から `_B_PUTC` 本体の入口を逆算します。どちらも、署名はリロケーションを含まない先頭 8 byte。

| ドライバ | `$1808` の指す先の署名 | 本体 = `$1808` − | 本体の署名（先頭 4 byte + 4 byte） | 採取 |
| --- | --- | --- | --- | --- |
| HIOCS.X 1.10+16 | `207a24e40c100001` | `0x30AC` | `10380990` + `66260cb8` | `CHAR d1 1`、`source = hiocs` |
| IOCS.X（SHARP 純正） | `207a05280c100001` | `0x2A9E` | `41ed0990` + `1010662e` | `CHAR d1 1`、`source = iocsx` |

- IOCS.X の本体の入口は、CONFIG.SYS 組み込みで `$05A134`（ロードベース `$059D6A`、ファイル内 `0x40A`）。IOCS のベクタ
  `$0480` は `$059F70`（本体の少し手前、`BSR $05A134`）。
- コマンドラインから組み込むと、どちらもユーザーメモリ（`$16xxxx` 付近）に載り、CONFIG.SYS 組み込みの位置とは大きく違います。
  走査範囲を固定した署名探索（上記の `0x50000`〜`0x70000`）は外れるが、`$1808` からの逆算は組み込み方法に依存しません。
- 解除（`HIOCS.X /R`、`IOCS.X /R`）で、`$1808` は元のハンドラ（`$8888`）、`$0480` は ROM のベクタへ戻る。
- CONDRV（`CONDRV.SYS -b<n>`、`-b` 無しは組み込みを中止する）を HIOCS.X の直前に組み込むと、HIOCS.X の本体の先頭 4 byte が
  `BRA.W`（`6000 xxxx`）に変わる。PC は同じなのでプローブは有効で、署名は先頭 4 byte を `6000` で始まる形も許します。
  IOCS.X と CONDRV の併用では書き換わりません。
- 本体の署名での走査は、1 MiB の範囲だと `TEXT PROBE` の有効化が返らないほど遅い。逆算したアドレスを直接使います。
- 常駐位置と署名はバージョンごとに違うので、別バージョンの HIOCS.X・IOCS.X には使えません。判別に失敗したときは自動では
  有効にならない（`text_probe(action="status")` の `AUTO=` で確認できる）。

Xtxt 0.58（12 ドットフォントのコンソールドライバ）は、`$0480` を自前の `_B_PUTC` に置き換え、12 ドットモードでは元のベクタ
（HIOCS.X の本体）へ処理を渡しません。HIOCS.X が常駐していても、DOS 出力は Xtxt の入口へ入り、HIOCS.X の本体は通らないため、
`hiocs` プロファイル・自動有効化のどちらでも採れません。自動有効化の対象外とし、必要なら Xtxt の `_B_PUTC` の 12 ドット側の入口
（フラグ判定の直後）に別のプロファイルを作る。Xtxt 0.58 は Human68k 3.01 専用で、3.02 では起動時に拒否されます。

採取の注意:

- `size = 2`（`D1.W` 全体）にすると、HIOCS の高速経路が 1 byte ずつ本体を呼ぶため、ASCII に `0x00` が付き、
  日本語（Shift_JIS）が壊れます。`size = 1` にします。
- D1.W に漢字の 2 byte 値を渡して `_B_PUTC` を直接呼ぶ IOCS 呼出しは、下位 1 byte しか採れません。
  HIOCS の高速経路はバイト単位なので、通常のコマンド出力には影響しません。
- `text_probe` が有効な間は `performance_counter` の区間登録ができません。測定の前に `text_probe(action="clear")` で無効にします。
- HIOCS.X のバージョンが変わったら、`[target] sha256` と入口署名を更新します。

確認例: 68000 / 10MHz、`disk/SYSTEM_HD.HDS`（HIOCS あり）で `PROCESS` を実行し、`read_text_events` の
`probe: B_PUTC_body` に、入力行から表の全行（日本語の見出しを含む）まで取れました。
