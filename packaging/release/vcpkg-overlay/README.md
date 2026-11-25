# vcpkg overlay port（Linux の Release ZIP 用 SDL3）

vcpkg 標準の `sdl3` ポートは、PulseAudio・PipeWire・ALSA を無効にしてビルドする。そのため、標準のポートで
作った Linux 用の SDL3 には音声出力のバックエンドが無く、ix68k は起動ログに
`SDL_InitSubSystem(AUDIO) failed: No available audio device` と出して、音が出ない（Issue #274）。

この overlay port は、標準の `sdl3` ポートを次の点だけ変えたもの。

- Linux のときだけ `-DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON`（実行時に `libpulse.so.0` を動的に読み込む。
  SDL3 自体は libpulse に依存しない）。
- `vcpkg.json` に `"port-version": 1` を追加。

PipeWire と ALSA は無効のまま。WSLg・Ubuntu・Debian など、PulseAudio（または `pipewire-pulse`）のある環境で音が出る。
利用者の環境に必要なのは `libpulse0` だけ（配布 README の記述のとおり）。

## 使い方

ビルドする機械に `libpulse-dev`（ヘッダと `libpulse.pc`）が要る。

```bash
sudo apt install -y libpulse-dev
VCPKG_BINARY_SOURCES=clear vcpkg install \
    --overlay-ports=<リポジトリ>/packaging/release/vcpkg-overlay sdl3:x64-linux-dynamic
```

`VCPKG_BINARY_SOURCES=clear` は、別の置き場所・別のポートで作った SDL3 が、バイナリキャッシュから復元されない
ようにするために付ける（`docs/ix68k-internals/build.md` の「Linux」参照）。overlay のポートはファイルの内容が違うので
ABI も変わり、標準のポートのキャッシュは復元されないが、念のため付ける。

sudo が使えない機械では、`apt download libpulse-dev` で取得した `.deb` を `dpkg -x` で展開し、`libpulse.pc` の
`prefix`・`libdir`・`includedir` を展開先に書き換えて `PKG_CONFIG_PATH` を向ける。vcpkg は環境変数を引き継がないので、
`VCPKG_KEEP_ENV_VARS=PKG_CONFIG_PATH` も付ける（`libpulse.so` は、システムの `libpulse.so.0` へのリンクを置く）。

## 確認

`packaging/release/stage.sh` は、ZIP の `lib/libSDL3.so.0` に `libpulse.so.0` の文字列（動的読み込みの名前）が
無いと、検証で止まる。手で確認するときは次のとおり。

```bash
strings -a lib/libSDL3.so.0 | grep libpulse.so.0
```

## 由来とライセンス

`ports/sdl3` は microsoft/vcpkg（MIT License、`LICENSE-vcpkg.txt`）のもの。コミット
`04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4` の `sdl3`（3.4.16）をコピーして、上の変更を加えた。
vcpkg を更新して `sdl3` の版が上がったときは、上流のポートとの差分を取り直す。
