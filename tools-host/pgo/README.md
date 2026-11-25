# PGO プロファイルの採取

設計: `docs/ix68k-internals/design/pgo_design.md`。対象は macOS arm64（Apple clang）と Windows x64（clang-cl）。
ROM・ディスク・GCC.X が揃った環境で、メンテナが行う。

リリース時は `packaging/release/stage.sh` が下の手順を自動で行い、同梱プロファイルを
更新してからビルドする（`local/profiles/` が無い環境、または `X68K_PGO_TRAIN=0` では省略）。
手動で採るときは次のとおり。

```sh
cmake -S . -B build/pgo-gen -DCMAKE_BUILD_TYPE=Release -DX68K_PGO_GENERATE=ON
cmake --build build/pgo-gen --parallel --target ix68k
python3 tools-host/pgo/train.py build/pgo-gen/ix68k packaging/release/pgo/ix68k-macos-arm64.profdata
```

その後、`packaging/release/pgo/README.md` に採取情報（採取日、ix68k と mc68000 のコミット、
コンパイラの版、不一致の件数）を書いてコミットする。
再生成が必要なのは、`extern/cmc68000/src/cpu/` のコア主要関数が変わったとき（設計書 6.4）。

通常ビルド（Clang の Release / RelWithDebInfo）は、同梱プロファイルがあれば自動で適用する
（不一致の警告は出さない）。止めるには `-DX68K_PGO_USE_BUNDLED=OFF`。

## Windows（clang-cl）

`packaging\release\stage.ps1` が、clang-cl のときに同じ手順を自動で行う（`local\profiles\` が無い環境、
または `$env:X68K_PGO_TRAIN=0` では省略）。手動で採るときは、VS の開発者環境と `clang-cl` が PATH にある状態で次のとおり。

```powershell
cmake -S . -B build_win_pgo_gen -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DX68K_PGO_GENERATE=ON -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build_win_pgo_gen --target ix68k
python tools\pgo\train.py build_win_pgo_gen\ix68k.exe packaging\release\pgo\ix68k-windows-x64.profdata
```

- MCP は TCP（`127.0.0.1:26800`、`IX68K_MCP_PORT` で変更）。ix68k が動いていると中止する
- 終了は `taskkill /PID`（`/F` なし）でウィンドウを閉じる要求を送り、正常終了で `.profraw` を書かせる
- `tests\work\gccbench` が無い環境では、`disk\GCCBENCH.XDF` のコピーを FD0 に挿して A: から実行する（元の XDF は変更しない）
- `llvm-profdata` は PATH か `C:\Program Files\LLVM\bin` から探す（`winget install LLVM.LLVM`）
