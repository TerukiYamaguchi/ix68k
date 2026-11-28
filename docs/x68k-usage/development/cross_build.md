# ホスト側クロスビルド

クロスビルドとは、実行先とは異なるコンピュータでプログラムをビルドする方法です。この場合は、
macOS などのホストで X68000（Human68k）用の `.X` 実行ファイルを作成します。ホスト上の高速な
エディタ、Git、コンパイラを使いながら、完成した `.X` だけを X68000 で動作確認できます。

新規アプリは、まず `m68k-xelf-gcc` と `elf2x68k` を使うクロスビルドを標準とします。ネイティブ
GCC.X との切り替え、ヘッダ、文字コードの差は
[ネイティブビルドとクロスビルドを切り替える](switching_build_environments.md) を参照してください。

## 1. 必要なもの

- ホスト上の C ソースと Makefile
- `m68k-xelf-gcc`: Motorola 68000 向けの C コンパイラ
- `elf2x68k.py`: ELF 実行ファイルを Human68k の `.X` 形式へ変換するツール
- Python 3 と `make`
- 動作確認用の ix68k と Human68k 環境

このリポジトリで確認済みの導入手順は macOS + Homebrew です。Windows では、elf2x68k 公式が配布する
MSYS2 MinGW 64bit 向けバイナリを使えます。WSL2 上の Ubuntu は Windows のネイティブ環境ではなく、
Linux 向けバイナリを使う別の方法です。

```sh
brew install yunkya2/tap/elf2x68k
command -v m68k-xelf-gcc
python3 "$(brew --prefix elf2x68k)/bin/elf2x68k.py" --help
```

最後の二つが成功すれば、コンパイラと変換ツールを利用できます。既存の Makefile は
`/opt/homebrew/opt/elf2x68k/bin/elf2x68k.py` を指定しています。Homebrew の導入先が異なる場合は、
`brew --prefix elf2x68k` の結果に合わせて `ELF2X68K` を変更してください。

### Windows（MSYS2 MinGW 64bit）

Windows では、[elf2x68k の公式 Releases](https://github.com/yunkya2/elf2x68k/releases) から
MSYS2 MinGW 64bit 用アーカイブを取得します。MSYS2 を導入し、**MSYS2 MinGW x64** シェルを開いてから
次の手順を実行してください。通常の `cmd.exe`、PowerShell、MSYS シェルではなく、MinGW x64 シェルを
使います。

1. アーカイブを、例えば `C:\x68k\elf2x68k` へ展開します。
2. 展開した `m68k-xelf\bin` を、MSYS2 シェルの `PATH` へ追加します。

   ```sh
   export PATH="/c/x68k/elf2x68k/m68k-xelf/bin:$PATH"
   command -v m68k-xelf-gcc
   m68k-xelf-gcc --version
   ```

3. 毎回の設定を避けるには、この `export` 行を使用中の MinGW x64 シェルの起動設定へ追加します。

公式配布の Windows 用ツールチェインは、`m68k-xelf-gcc` から直接 `.X` を生成します。したがって、
Windows では次のようにビルドします。

```sh
m68k-xelf-gcc -m68000 -O2 -Wall -Wextra -o HELLO.X hello.c
```

`HELLO.X` と、中間生成物の `HELLO.X.elf` が作成されます。リンク時は `m68k-xelf-ld` を直接呼ばず、
必ず `m68k-xelf-gcc` を使ってください。

## 2. ソースコードを書くときの注意

### CPU と ABI

通常は X68000 の標準 CPU に合わせて `-m68000` を指定します。68020 以降の命令や、その CPU 固有の
機能を使う場合だけ、対象機種と動作確認環境を明示したうえで変更してください。

標準ライブラリ、コマンドライン引数、`printf` を使う通常の C プログラムは、標準スタートアップを
使います。`-nostdlib` や `-nostartfiles` を付けるのは、独自の `_start` を持つ小さなアセンブリ
プログラムなど、起動処理を自分で用意する場合だけです。

### ヘッダと文字コード

DOSCALL を使う場合、クロスビルドでは次のヘッダを使います。

```c
#include <x68k/dos.h>
```

ホスト側のソースは UTF-8 を正本にします。ネイティブ GCC.X でもビルドする場合は、CP932 へ変換した
ファイルを別に作成し、`doslib.h` へ切り替えます。詳しい往復手順は
[ネイティブビルドとクロスビルドを切り替える](switching_build_environments.md) を参照してください。

出力ファイル名は、Human68k で扱いやすい 8.3 形式の大文字（例: `HELLO.X`）を推奨します。ホスト側の
パスやファイル名には、ゲストで扱えない文字を使わないでください。

### 警告を有効にする

最初から `-Wall -Wextra` を付け、警告を残したまま成果物を配布しないでください。ポインタ幅、
整数型のサイズ、構造体のレイアウトはホスト向けプログラムと同じとは限りません。ホスト上だけで
実行するテストに依存せず、必ず X68000 上でも動作確認します。

## 3. ビルドする

### まずはコマンドで実行する

`hello.c` から `HELLO.X` を作る最小例です。

```sh
m68k-xelf-gcc -m68000 -O2 -Wall -Wextra -o hello.elf hello.c
python3 "$(brew --prefix elf2x68k)/bin/elf2x68k.py" -o HELLO.X hello.elf
```

最初のコマンドは X68000 向けの ELF を作り、二つ目のコマンドが Human68k で実行できる `.X` に変換します。
`hello.elf` は中間生成物なので、通常は Git に追加しません。

### Makefile を使う

繰り返しビルドするプログラムは Makefile にします。次は標準スタートアップを使う単一 C ファイル用の
最小例です。

```make
CROSS = m68k-xelf-
CC = $(CROSS)gcc
CFLAGS = -m68000 -O2 -Wall -Wextra
ELF2X68K = $(shell brew --prefix elf2x68k)/bin/elf2x68k.py

all: HELLO.X

HELLO.X: hello.elf
	python3 $(ELF2X68K) -o $@ $<

hello.elf: hello.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f hello.elf HELLO.X

.PHONY: all clean
```

```sh
make
```

実際の構成は [GhostED の Makefile](https://github.com/TerukiYamaguchi/ix68k/blob/main/tools-x68k/ged/Makefile) を参照してください。複数の
ソースファイルでは、各 `.c` を `.o` にコンパイルしてからリンクする規則を追加します。

Windows 公式配布版用の Makefile では、ELF→X の変換をコンパイラドライバへ任せます。

```make
CROSS = m68k-xelf-
CC = $(CROSS)gcc
CFLAGS = -m68000 -O2 -Wall -Wextra

all: HELLO.X

HELLO.X: hello.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: all
```

## 4. X68000 へ導入して実行する

開発中は HOSTDRV による共有を推奨します。ホスト側の作業ディレクトリを `hostdrv_dir` に設定し、
`HOSTDRV.SYS` を導入すると、生成した `HELLO.X` を共有ドライブからすぐ実行できます。設定と導入の
詳細は [HOSTDRV ユーザーズマニュアル](../guide/hostdrv.md) を参照してください。

共有ドライブが `A:` に割り当てられた例では、ゲスト側で次のように実行します。

```text
A:
HELLO
```

ドライブ文字は環境によって異なります。起動時に表示された HOSTDRV の割当先を使ってください。

配布物のように固定 HDS へ配置する場合は、ix68k を停止してから `hu68disk` を使います。

```sh
tools-host/hu68disk/hu68disk copy disk/SYSTEM_HD.HDS HELLO.X BIN/HELLO.X
```

稼働中の HDS にホスト側から直接書き込んでも、Human68k の FAT キャッシュにはすぐ反映されません。
HDS を更新した後は ix68k を再起動してください。単一ファイルだけを転送する場合は HOSTFILE も使えます。
用途ごとの選び方は [ホストとゲストのファイル転送](../guide/file_transfer.md) を参照してください。

## 5. 確認と困ったとき

1. `make` が警告なく完了し、`.X` が生成されることを確認します。
2. X68000 で `.X` を実行し、終了コード、画面出力、ファイル入出力を確認します。
3. ファイルが見えない場合は、HOSTDRV の割当ドライブ、`hostdrv_dir`、現在のディレクトリを確認します。
4. `m68k-xelf-gcc: command not found` の場合は、macOS では Homebrew の導入結果、Windows では
   `m68k-xelf/bin` を追加した `PATH` を確認します。
5. `elf2x68k.py` が見つからない場合は、`brew --prefix elf2x68k` でインストール先を確認し、Makefile の
   `ELF2X68K` を修正します。

転送・実行・MCP による確認方法は [test_and_debug.md](test_and_debug.md) を参照してください。
