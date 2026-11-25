# PGO プロファイル

`stage.sh` は、`local/profiles/` があればこのプロファイルを採り直してから、リリースビルドに適用する。
通常ビルド（CMake、Clang の Release 系）も、このファイルがあれば適用する。
採取手順は `tools-host/pgo/README.md`、設計は `docs/ix68k-internals/design/pgo_design.md`。

下の表のコミットは、公開前の旧リポジトリ（x68000・mc68000）のものです。新しいリポジトリは履歴を引き継いでいないため、
このハッシュは辿れません。採取した時点の目印として残しています。

## ix68k-macos-arm64.profdata

| 項目 | 値 |
| --- | --- |
| 採取日 | 2026-11-13 |
| ix68k のコミット | b98b0ba |
| mc68000 のコミット | e963d54 |
| コンパイラ | Apple clang 21.0.0 (clang-2100.4.1.1) |
| 学習内容 | GCC.X BENCH 1 回（`disk.cfg` + `x68060.cfg`、68060/50MHz、IC/DC 既定 ON） |
| ix68k ターゲットの不一致 | 0 件（`profile-instr-out-of-date`） |
| 効果（BENCH ホスト CPU 秒） | 標準 約 30.7 秒 → 約 24.5 秒。hard-overload は標準 約 1,120 フレーム → 0 |

## ix68k-windows-x64.profdata

clang-cl 用（MSVC は Clang 判定から外れるので適用されない）。

| 項目 | 値 |
| --- | --- |
| 採取日 | 2026-11-25 |
| ix68k のコミット | 621142d |
| mc68000 のコミット | 62c441f |
| コンパイラ | clang-cl 23.1.5（winget の LLVM.LLVM）、Release（`/O2 /Ob2`）、LTCG なし |
| 学習内容 | GCC.X BENCH 1 回（`disk.cfg` + `x68060.cfg`、68060/50MHz）。`disk/GCCBENCH.XDF` のコピーを MCP の `DISK INSERT` で A: に挿して実行。終了は `taskkill /PID`（`/F` なし）でウィンドウを閉じる |
| ix68k ターゲットの不一致 | 0 件（`profile-instr-out-of-date`） |
| 効果（BENCH ホスト CPU 秒） | 2026-11-16 採取分の測定値: PGO なし 平均 43.11 秒 → 平均 37.99 秒（約 12% 減）。MSVC 平均 52.34 秒に対して約 27% 減。hard-overload は約 1,650 フレーム残る（11-25 の再採取では未測定） |

`stage.ps1` は clang-cl があればそれを使い、このプロファイルをリリース ZIP のビルドに適用する。

## ix68k-linux-x64.profdata

Linux x86_64 の clang 用（GCC では適用されない。`stage.sh` は clang が無いと PGO なしで作る）。

| 項目 | 値 |
| --- | --- |
| 採取日 | 2026-11-25 |
| ix68k のコミット | 621142d |
| mc68000 のコミット | 62c441f |
| コンパイラ | clang 23.1.5（LLVM 公式の Linux-X64 プリビルド）、Release（`-O3`）。環境は WSL2 の Ubuntu 22.04 |
| 学習内容 | GCC.X BENCH 1 回（`disk.cfg` + `x68060.cfg`、68060/50MHz）。`SDL_VIDEODRIVER=dummy`、`SDL_AUDIODRIVER=dummy`。終了は SIGTERM |
| ix68k ターゲットの不一致 | 0 件（`profile-instr-out-of-date`） |
| 効果（BENCH ホスト CPU 秒、3 回の平均） | 2026-11-16 採取分の測定値（11-25 の再採取では未測定。SDL3 は vcpkg の 3.4.16 共有版でビルド）: GCC 11.4 の PGO なし 46.9 秒、clang の PGO なし 42.0 秒、clang の PGO あり 37.7 秒（clang の PGO なしに対して約 10% 減、GCC に対して約 20% 減） |

hard-overload（BENCH 区間の合計フレーム、3 回）は、GCC 1,616〜1,647、clang の PGO なし 1,662〜1,679、clang の PGO あり 1,624〜1,694 で、PGO でも減らず約 1,650 フレーム残る（dummy ドライバでの測定。PGO あり 3 回の CPU 秒は 37.51〜39.42 秒）。

SDL3 は apt に無い（22.04）ので、release-3.4.20 をソースビルドして測った。PGO の速度の測定は X11 のみ・音声バックエンドなしの SDL3 で行った。
配布用の確認には、X11・ALSA・PulseAudio・PipeWire を有効にした SDL3 でビルドした ZIP を使った（Wayland と KMSDRM は切った。22.04 の libwayland が古く、SDL 3.4.20 の Wayland がビルドできない）。
