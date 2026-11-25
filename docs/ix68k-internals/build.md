# ビルド・実行

この文書は、配布済みソースから ix68k をビルドして実行する利用者向けの手順です。

## 必要なもの

- C99コンパイラ、CMake 3.16以降、Git。
- GUI版には SDL3。SDL2 は非対応。
- X68000の IPLROM、CGROM、Human68kブートディスク。これらはリポジトリに同梱しません。
  入手方法と著作権上の注意は [README](../../README.md) を参照します。

ソースをGitで取得する場合は、MC68000コアとゲームパッド定義を含めてサブモジュールも取得します。

```bash
git clone --recurse-submodules https://github.com/TerukiYamaguchi/ix68k.git
cd ix68k

# 既存cloneでサブモジュールが未取得の場合
git submodule update --init --recursive
```

## ROM・ディスク・設定

実行には少なくともIPLROMとCGROMが必要です。次の配置を例とします。

```text
rom/IPLROM.DAT       X68000のIPLROM
rom/CGROM.DAT        CGROM
disk/HUMAN302.XDF    Human68k v3.02のブートディスク（XDF）
```

`CGROM.DAT` は、権利上問題のない手元フォントから
[`tools-host/cgrom_gen/`](../../tools-host/cgrom_gen/) で生成できます。初回は設定例をコピーし、
ROM・CGROM・ディスクのパスを環境に合わせて編集します。

```bash
cp profiles/ix68k.cfg.example ix68k.cfg
```

以後は設定ファイルを自動読込みするため、通常は `./build/ix68k` だけで起動できます。
`-rom` と `-cgrom` で明示指定する場合は、設定ファイルを作らずに起動できます。

## ヘッドレス版

画面・音声入力出力が不要な自動テスト、MCP操作、サーバー用途では SDL3 を必要としません。
GUI版とは別のビルドディレクトリを使います。

```bash
cmake -S . -B build-nogui -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPLATFORM_NULL=1
cmake --build build-nogui --parallel
ctest --test-dir build-nogui --output-on-failure

./build-nogui/ix68k-nogui -rom rom/IPLROM.DAT
```

ヘッドレス版の実行ファイル名は `ix68k-nogui` です。CGROMは描画しないため必須ではありません。

## macOS

```bash
brew install cmake sdl3
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure

./build/ix68k -rom rom/IPLROM.DAT -cgrom rom/CGROM.DAT
```

## Linux（Ubuntu / Debian系）

Ubuntu 22.04/24.04の標準APTにはSDL3開発パッケージがないため、GUI版はvcpkgでSDL3を導入します。
SDL3は共有版（`x64-linux-dynamic`トリプレット）を使います。Release ZIP（`packaging/release/stage.sh`）が
`libSDL3.so`を同梱するためで、普段のビルドも同じ構成にしておくと、ZIPでだけ起きる問題を避けられます。
CIでもUbuntu上のGUI版とヘッドレス版をビルド・テストしています。

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    autoconf autoconf-archive automake libtool zip unzip python3-venv \
    libx11-dev libxft-dev libxext-dev libgl1-mesa-dev libpulse-dev

git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
./vcpkg/vcpkg install --overlay-ports="$PWD/packaging/release/vcpkg-overlay" sdl3:x64-linux-dynamic

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET=x64-linux-dynamic
cmake --build build --parallel
ctest --test-dir build --output-on-failure

./build/ix68k -rom rom/IPLROM.DAT -cgrom rom/CGROM.DAT
```

**SDL3は、`packaging/release/vcpkg-overlay`のoverlay portで入れます。** vcpkg標準の`sdl3`ポートは
PulseAudioを無効にしてビルドするため、標準のポートで作ったSDL3では、Linux版が`SDL_InitSubSystem(AUDIO)
failed: No available audio device`と出して音が出ません（Issue #274）。overlay portはLinuxだけPulseAudioを
有効にします（実行時に`libpulse.so.0`を動的に読み込むので、利用者に要るのは`libpulse0`だけです）。
ビルドする機械には`libpulse-dev`が要ります。sudoが使えない機械での手順は、
[`packaging/release/vcpkg-overlay/README.md`](../../packaging/release/vcpkg-overlay/README.md)にあります。
`stage.sh`は、ZIPの`lib/libSDL3.so.0`に`libpulse.so.0`の文字列が無いと、検証で止まります。

vcpkgを別の場所へ置く場合は、`CMAKE_TOOLCHAIN_FILE` にその絶対パスを指定します。
Release ZIPは`CMAKE_TOOLCHAIN_FILE=<vcpkg.cmake> packaging/release/stage.sh`で作ります（トリプレットは
既定で`x64-linux-dynamic`。`X68K_VCPKG_TRIPLET`で変更できます）。
**配布するZIPを作るときは、vcpkgを`$HOME`の外（例: `/var/tmp/vcpkg`）に置き、
`VCPKG_BINARY_SOURCES=clear`を付けてSDL3をソースからビルドします**
（`VCPKG_BINARY_SOURCES=clear /var/tmp/vcpkg/vcpkg install --overlay-ports=<リポジトリ>/packaging/release/vcpkg-overlay sdl3:x64-linux-dynamic`。
`stage.sh`の実行時にも同じ環境変数を付けます）。vcpkgがSDL3をビルドするとき、`__FILE__`由来の
ビルドパス（`/home/<ユーザー名>/vcpkg/buildtrees/...`）が`libSDL3.so`に埋め込まれ、ユーザー名が
配布物に入るためです。vcpkgの置き場所を変えただけでは、`$HOME`の下のバイナリキャッシュ
（`~/.cache/vcpkg/archives`）から、パスの入った旧いSDL3が復元されることがあります。
`stage.sh`は、ZIPの中に`$HOME`・`/home/<ユーザー名>/`・`/Users/<ユーザー名>/`を含むファイルが
あると、検証で止まります（`stage.ps1`は`%USERPROFILE%`などを検査します）。
Wayland環境でもSDL3のバックエンドが選ばれるが、問題の切り分けにはX11セッションでの起動を試します。
WSLg（既定のWaylandバックエンド）では、描画とMCP tickが同一メインループにあるため
レンダリングが数十秒単位で滞ると外部からのMCP要求もtimeoutします。`SDL_VIDEODRIVER=x11`
を指定すると解消する（Linux上の検証で確認済み）。

## Windows

Windows 10 1803以降では Visual Studio Build Tools 2022、CMake、vcpkg版SDL3でネイティブビルド
できます。MSVCの開発者コマンドプロンプトで、vcpkgを準備してから構成します。

```bat
git clone https://github.com/microsoft/vcpkg.git
vcpkg\bootstrap-vcpkg.bat
vcpkg\vcpkg install sdl3

cmake -S . -B build_win -G "NMake Makefiles" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_TOOLCHAIN_FILE=%CD%\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build_win --parallel
ctest --test-dir build_win --output-on-failure
```

実行ファイルは `build_win\ix68k.exe` です。WindowsのMCPはAF_UNIXではなく
`127.0.0.1:26800` のTCPループバックを使います。ポートは `IX68K_MCP_PORT` で変更できます。

## ビルド種別とトラブルシュート

- 通常実行と性能確認には `RelWithDebInfo` を使います。`Debug` は実時間60fpsを保証しません。
- GUI版のCMake構成でSDL3が見つからない場合は、SDL3のCMakeパッケージ設定を含むプレフィックス、
  またはvcpkgの `CMAKE_TOOLCHAIN_FILE` を指定します。
- ビルドディレクトリは環境ごとに作り直してよい。構成を切り替える際は、GUI版とヘッドレス版で
  同じビルドディレクトリを共有しません。
- アドレス・周辺機器の対応状況は [addrmap.md](addrmap.md)、起動媒体の設定は
  [boot.md](boot.md) を参照します。
