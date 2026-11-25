#!/usr/bin/env bash
# Release ZIP をローカルで組み立てて検証する（Issue #195）。
# 同梱物の正本は packaging/release/release_package_contents.md。
# 対応するホストは macOS（arm64 / x64）と Linux（x86_64。配布は ubuntu-22.04 で作る）。
# Windows は packaging/release/stage.ps1。
#
# 使い方: packaging/release/stage.sh [バージョン]
#   バージョン省略時は実行日（YYYYMMDD）。出力は build/release/ix68k-<os>-<arch>-<バージョン>.zip
#   （ZIP 内のトップディレクトリは日付なしの ix68k-<os>-<arch>）。
#   X68K_BUILD_JOBS=N でビルドの並列数を指定（既定は CPU 数）。
#   local/profiles/ があれば PGO プロファイルを採り直す（X68K_PGO_TRAIN=0 で省略）。
set -euo pipefail

readonly VERSION="${1:-$(date +%Y%m%d)}"
readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly OUT_DIR="$ROOT/build/release"
readonly BUILD_DIR="$OUT_DIR/cmake-build"
readonly VENV_DIR="$OUT_DIR/venv"
readonly PYI_DIR="$OUT_DIR/pyinstaller"
readonly HOSTDRV_DIR="$ROOT/tools-x68k/hostdrv"
readonly HOSTFILE_X="$ROOT/tools-x68k/hostfile/hostfile.x"
readonly JOYGET_TEST_X="$ROOT/tools-x68k/joyget_test/joyget_test.x"
readonly GED_X="$ROOT/tools-x68k/ged/GED.X"
readonly AIQ_X="$ROOT/tools-x68k/aiq/aiq.x"
readonly FNTADR_X="$ROOT/tools-x68k/fntadr/fntadr.x"
readonly TOOLS_HOST_DIR="tools-host"
readonly TOOLS_X68K_DIR="tools-x68k"
# ZIPに含めてはならないROM・メディアの拡張子（大文字小文字を区別しない）
# ZIPに入れる例外: フォーマット済みの空2HDイメージ（ROM・Human68kを含まない雛形）
readonly BLANK_XDF="$ROOT/packaging/release/BLANK.XDF"
readonly BLANK_XDF_ZIP_REGEX='/disk/BLANK\.XDF$'
readonly FORBIDDEN_EXT_REGEX='\.(dat|rom|xdf|hds|hdf|dim|2hd|d88|iso)$'

log() { echo "[stage] $*"; }
die() { echo "[stage] ERROR: $*" >&2; exit 1; }

detect_package_name() {
    case "$(uname -s)/$(uname -m)" in
        Darwin/arm64) echo "ix68k-macos-arm64" ;;
        Darwin/x86_64) echo "ix68k-macos-x64" ;;
        Linux/x86_64) echo "ix68k-linux-x64" ;;
        *) die "未対応のホスト: $(uname -s)/$(uname -m)" ;;
    esac
}

is_linux() { [[ "$(uname -s)" == "Linux" ]]; }

# ビルドの並列数。メモリの小さい環境（WSL 等）で OOM になるときは X68K_BUILD_JOBS=4 などで絞る
readonly BUILD_JOBS="${X68K_BUILD_JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"

# PGO は Clang 専用（プロファイル形式が GCC と別。Issue #268）。
# macOS は Apple clang が既定。Linux は CC（環境変数）→ PATH 上の clang の順に探し、
# 無ければ警告して既定コンパイラ（通常は GCC）で PGO なしのビルドにする。
COMPILER_ARGS=()
USE_PGO=1
# vcpkg のトリプレット。Linux の Release ZIP は共有版 libSDL3.so を同梱するため、
# 既定の x64-linux（静的）ではなく x64-linux-dynamic を使う（X68K_VCPKG_TRIPLET で変更可）。
# CMAKE_TOOLCHAIN_FILE を渡したときだけ有効。
TRIPLET_ARGS=()
if [[ -n "${CMAKE_TOOLCHAIN_FILE:-}" ]]; then
    if is_linux; then
        TRIPLET_ARGS=(-DVCPKG_TARGET_TRIPLET="${X68K_VCPKG_TRIPLET:-x64-linux-dynamic}")
    elif [[ -n "${X68K_VCPKG_TRIPLET:-}" ]]; then
        TRIPLET_ARGS=(-DVCPKG_TARGET_TRIPLET="$X68K_VCPKG_TRIPLET")
    fi
fi
select_compiler() {
    if ! is_linux; then
        return
    fi
    local cc="${CC:-}"
    [[ -z "$cc" ]] && cc="$(command -v clang || true)"
    if [[ -n "$cc" ]] && "$cc" --version 2>/dev/null | grep -qi 'clang'; then
        COMPILER_ARGS=(-DCMAKE_C_COMPILER="$cc")
        log "コンパイラ: ${cc}（PGO 有効）"
    else
        USE_PGO=0
        log "WARN: clang が見つからないため PGO なしでビルドする（apt install clang llvm、または CC=<clang>）"
    fi
}

# リリースビルドの前に PGO プロファイルを採り直す（設計: design/pgo_design.md）。
# 採取には ROM・ディスク・GCC.X を使う local/profiles/ が要る。無い環境では採取せず、
# リポジトリ同梱のプロファイルを使う。X68K_PGO_TRAIN=0 でも採取を省く。
# 採取に失敗したら、古いプロファイルで配布物を作らないよう中止する。
train_pgo_profile() {
    if [[ "${X68K_PGO_TRAIN:-1}" == "0" ]]; then
        log "PGO プロファイルの採取を省略（X68K_PGO_TRAIN=0）"
        return
    fi
    if [[ "$USE_PGO" != "1" ]]; then
        log "PGO 採取を省略（clang が無い）"
        return
    fi
    if [[ ! -f "$ROOT/local/profiles/disk.cfg" ]]; then
        log "PGO 採取の前提（local/profiles/disk.cfg）が無いため、同梱プロファイルを使う"
        return
    fi
    local gen_dir="$OUT_DIR/pgo-gen"
    local profile="$ROOT/packaging/release/pgo/$(detect_package_name).profdata"
    local fresh="$OUT_DIR/fresh.profdata"
    log "PGO プロファイルを採取する（計装ビルド → GCC.X BENCH）"
    # 古いキャッシュのコンパイラが残らないよう、毎回作り直す
    rm -rf "$gen_dir"
    # SDL の映像・音声は無くてよい（学習はヘッドレスで足りる。Linux の CI/WSL 向け）
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
    cmake -S "$ROOT" -B "$gen_dir" -DCMAKE_BUILD_TYPE=Release -DX68K_PGO_GENERATE=ON \
        ${COMPILER_ARGS[@]+"${COMPILER_ARGS[@]}"} ${TRIPLET_ARGS[@]+"${TRIPLET_ARGS[@]}"}         ${CMAKE_TOOLCHAIN_FILE:+-DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE"} > /dev/null
    cmake --build "$gen_dir" --parallel "$BUILD_JOBS" --target ix68k > /dev/null
    rm -f "$fresh"
    python3 "$ROOT/tools-host/pgo/train.py" "$gen_dir/ix68k" "$fresh" || die "PGO プロファイルの採取に失敗"
    mkdir -p "$(dirname "$profile")"
    mv "$fresh" "$profile"
    log "PGO プロファイルを更新: ${profile}（packaging/release/pgo/README.md の採取情報も更新してコミットすること）"
}

build_and_install() {
    local install_dir="$1"
    train_pgo_profile
    local extra=(${COMPILER_ARGS[@]+"${COMPILER_ARGS[@]}"})
    # Linux: 実行ファイルを展開先トップへ置き、SDL3を lib/ へ同梱するレイアウト
    is_linux && extra+=(-DX68K_RELEASE_LAYOUT=ON)
    # SDL3を vcpkg 等から取るときは CMAKE_TOOLCHAIN_FILE を環境変数で渡せる
    [[ -n "${CMAKE_TOOLCHAIN_FILE:-}" ]] && extra+=(-DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" ${TRIPLET_ARGS[@]+"${TRIPLET_ARGS[@]}"})
    # PGO: パッケージ名のプロファイルがあれば適用する（設計: design/pgo_design.md）
    local profile="$ROOT/packaging/release/pgo/$(detect_package_name).profdata"
    if [[ "$USE_PGO" != "1" ]]; then
        log "PGO 無し（clang が無い）"
    elif [[ -f "$profile" ]]; then
        log "PGO プロファイルを適用: $profile"
        extra+=(-DX68K_PGO_PROFILE="$profile")
    else
        log "PGO プロファイル無し（通常ビルド）"
    fi
    cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DX68K_DEV_DOCK_ICON=OFF \
        -DX68K_APP_VERSION="$VERSION" -DX68K_RELEASE_VERSION="$VERSION" ${extra[@]+"${extra[@]}"} > /dev/null
    # 配布する ix68k だけを先にビルドして不一致を数える（テスト用ターゲットは
    # 別の定義でコンパイルされるため、全体ビルドの警告数は指標にならない）
    local build_log="$OUT_DIR/build_ix68k.log"
    cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS" --target ix68k 2>&1 | tee "$build_log"
    if [[ "$USE_PGO" == "1" && -f "$profile" ]]; then
        log "ix68k の profile-instr-out-of-date 警告: $(grep -c 'profile-instr-out-of-date' "$build_log" || true) 件"
    fi
    cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"
    ctest --test-dir "$BUILD_DIR" --output-on-failure
    cmake --install "$BUILD_DIR" --prefix "$install_dir" > /dev/null
}

# HOSTDRV.SYS はGit管理のビルド済みバイナリ（ホスト非依存）。クロスツールチェインは不要。
check_hostdrv() {
    [[ -f "$HOSTDRV_DIR/HOSTDRV.SYS" ]] || die "HOSTDRV.SYS が無い: $HOSTDRV_DIR"
    [[ -f "$HOSTFILE_X" ]] || die "hostfile.x が無い: $HOSTFILE_X"
    local f
    for f in "$JOYGET_TEST_X" "$GED_X" "$AIQ_X" "$FNTADR_X"; do
        [[ -f "$f" ]] || die "ゲスト用バイナリが無い: $f"
    done
}

# tools-x68k/<dir>/ へゲスト用バイナリと README.TXT を置く
# 引数: ディレクトリ名 コピー元 ZIP内のファイル名。README.TXT（Shift-JIS・CRLF）はコピー元と同じディレクトリのもの。
stage_x68k_tool() {
    local pkg_dir="$1" dir="$2" src="$3" name="$4"
    mkdir -p "$pkg_dir/$TOOLS_X68K_DIR/$dir"
    cp "$src" "$pkg_dir/$TOOLS_X68K_DIR/$dir/$name"
    cp "$(dirname "$src")/README.TXT" "$pkg_dir/$TOOLS_X68K_DIR/$dir/README.TXT"
}

build_hostfile_cli() {
    local dest="$1"
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --quiet pyinstaller
    "$VENV_DIR/bin/pyinstaller" --onefile --name ix68k-hostfile \
        --distpath "$PYI_DIR/dist" --workpath "$PYI_DIR/work" --specpath "$PYI_DIR" \
        --paths "$ROOT/tools-host/mcp" --log-level WARN "$ROOT/tools-host/ix68k-hostfile/ix68k-hostfile"
    mkdir -p "$dest/$TOOLS_HOST_DIR/ix68k-hostfile"
    cp "$PYI_DIR/dist/ix68k-hostfile" "$dest/$TOOLS_HOST_DIR/ix68k-hostfile/"
    cp "$ROOT/tools-host/ix68k-hostfile/README.md" "$dest/$TOOLS_HOST_DIR/ix68k-hostfile/README.md"
}

# ホストOS用の補助ツール（macOS / Linux のみ。いずれもPOSIX前提）。
#   hu68disk    : ディスクイメージの操作。Makefileでビルドしたバイナリを入れる（ZIPにソースは入れない）
#   sram_switch : sram.dat のメモリスイッチ設定。同上
#   cgrom_gen   : ビットマップフォント(BDF)から互換CGROMを生成する。同上
#   aiq-bridge  : AIQ.X のホスト側ブリッジ。Python（標準ライブラリのみ）なのでスクリプトのまま入れる
stage_host_tools() {
    local pkg_dir="$1" dest="$pkg_dir/$TOOLS_HOST_DIR" tool
    for tool in hu68disk sram_switch cgrom_gen; do
        make -B -C "$ROOT/$TOOLS_HOST_DIR/$tool" > /dev/null
        mkdir -p "$dest/$tool"
        cp "$ROOT/$TOOLS_HOST_DIR/$tool/$tool" "$ROOT/$TOOLS_HOST_DIR/$tool/README.md" "$dest/$tool/"
    done
    mkdir -p "$dest/aiq-bridge"
    cp "$ROOT/$TOOLS_HOST_DIR/aiq-bridge/aiq_bridge.py" "$ROOT/$TOOLS_HOST_DIR/aiq-bridge/docs_search.py" \
        "$ROOT/$TOOLS_HOST_DIR/aiq-bridge/README.md" "$dest/aiq-bridge/"
    cp -R "$ROOT/$TOOLS_HOST_DIR/aiq-bridge/prompts" "$dest/aiq-bridge/"
}

# MCPサーバ一式（ビルドせずClaude Code等からix68kを操作する人向け）。
# リポジトリ直下の .mcp.json（tools-host/mcp を指す）をそのまま入れる（展開先で claude を起動すれば自動で読まれる）。
stage_mcp() {
    local pkg_dir="$1" dest="$pkg_dir/$TOOLS_HOST_DIR/mcp" f
    mkdir -p "$dest"
    for f in ix68k_mcp_server.py ix68k_wire.py pyproject.toml uv.lock MCP_TOOLS.md README.md; do
        cp "$ROOT/tools-host/mcp/$f" "$dest/"
    done
    cp -R "$ROOT/tools-host/mcp/text_profiles" "$dest/"
    cp "$ROOT/.mcp.json" "$pkg_dir/.mcp.json"
}

assemble_package() {
    local pkg_dir="$1" install_dir="$2"
    [[ -f "$BLANK_XDF" ]] || die "BLANK.XDF が無い: $BLANK_XDF"
    mkdir -p "$pkg_dir/rom" "$pkg_dir/disk" "$pkg_dir/docs"
    if is_linux; then
        cp -R "$install_dir/." "$pkg_dir/"
    else
        cp -R "$install_dir/ix68k.app" "$pkg_dir/"
    fi
    build_hostfile_cli "$pkg_dir"
    stage_x68k_tool "$pkg_dir" hostdrv "$HOSTDRV_DIR/HOSTDRV.SYS" HOSTDRV.SYS
    stage_x68k_tool "$pkg_dir" hostfile "$HOSTFILE_X" HOSTFILE.X
    stage_x68k_tool "$pkg_dir" joyget_test "$JOYGET_TEST_X" JOYGET_TEST.X
    stage_x68k_tool "$pkg_dir" ged "$GED_X" GED.X
    stage_x68k_tool "$pkg_dir" aiq "$AIQ_X" AIQ.X
    stage_x68k_tool "$pkg_dir" fntadr "$FNTADR_X" FNTADR.X
    cp "$BLANK_XDF" "$pkg_dir/disk/BLANK.XDF"
    stage_mcp "$pkg_dir"
    stage_host_tools "$pkg_dir"
    cp -R "$ROOT/LICENSES" "$pkg_dir/"
    cp "$ROOT/profiles/ix68k.cfg.example" "$ROOT/profiles/keyboard.cfg.example" "$pkg_dir/"
    cp "$ROOT/packaging/release/dist_readme.md" "$pkg_dir/README.md"
    cp -R "$ROOT/docs/x68k-usage" "$pkg_dir/docs/"
    if [[ -f "$ROOT/LICENSE" ]]; then
        cp "$ROOT/LICENSE" "$pkg_dir/"
    else
        log "WARN: LICENSE が未作成のため同梱していない"
    fi
    find "$pkg_dir" -name .DS_Store -delete
    # 開発機のxattr（quarantine等）がZIPへ混入しないようにする
    is_linux || xattr -cr "$pkg_dir"
}

make_zip() {
    local stage_root="$1" name="$2" zip_path="$3"
    rm -f "$zip_path"
    if is_linux; then
        # -X: 余分な属性を除外。同梱物にシンボリックリンクは無い（verify_linux_binary が確認する）。
        # zip は空ディレクトリ（rom/ disk/）もエントリとして保持する。
        (cd "$stage_root" && zip -q -r -y -X "$zip_path" "$name")
    else
        (cd "$stage_root" && ditto -c -k --norsrc --keepParent "$name" "$zip_path")
    fi
}

# Linux: SDL3が同梱の lib/ から解決され、ホストのSDL3へ依存していないことを確認する。
verify_linux_binary() {
    local pkg="$1"
    [[ -z "$(find "$pkg" -type l)" ]] || die "同梱物にシンボリックリンクがある: $(find "$pkg" -type l)"
    readelf -d "$pkg/ix68k" | grep -Eq 'RUNPATH|RPATH.*\$ORIGIN/lib' || die "RUNPATHに \$ORIGIN/lib が無い"
    local ldd_out
    ldd_out="$(LD_LIBRARY_PATH= ldd "$pkg/ix68k")"
    echo "$ldd_out" | grep -q 'not found' && { echo "$ldd_out"; die "未解決の共有ライブラリがある"; }
    echo "$ldd_out" | grep 'libSDL3' | grep -q "$pkg/lib/" || die "SDL3が同梱のlib/から解決されていない"
    # ROM無しで起動し、ライブラリ解決後にROM不在で正常終了（非0）することだけ確認する
    local out
    out="$(cd "$pkg" && HOME="$OUT_DIR/verify-home" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
        timeout 10 ./ix68k 2>&1 || true)"
    echo "$out" | grep -qi 'error while loading shared libraries' && die "ix68kがライブラリ解決に失敗: $out"
    return 0
}

verify_zip() {
    local zip_path="$1" name="$2"
    local check_dir="$OUT_DIR/verify"
    rm -rf "$check_dir"
    mkdir -p "$check_dir"
    unzip -q "$zip_path" -d "$check_dir"
    local pkg="$check_dir/$name"

    local required
    if is_linux; then
        required=("ix68k" "lib/libSDL3.so.0" "gamecontrollerdb.txt")
    else
        required=(
            "ix68k.app/Contents/MacOS/ix68k"
            "ix68k.app/Contents/Frameworks"
            "ix68k.app/Contents/Resources/gamecontrollerdb.txt"
        )
    fi
    required+=(
        "tools-host/ix68k-hostfile/ix68k-hostfile" "tools-host/ix68k-hostfile/README.md"
        "tools-x68k/hostdrv/HOSTDRV.SYS" "tools-x68k/hostfile/HOSTFILE.X" "tools-x68k/joyget_test/JOYGET_TEST.X"
        "tools-x68k/ged/GED.X" "tools-x68k/aiq/AIQ.X" "tools-x68k/fntadr/FNTADR.X"
        "tools-x68k/hostdrv/README.TXT" "tools-x68k/hostfile/README.TXT" "tools-x68k/joyget_test/README.TXT"
        "tools-x68k/ged/README.TXT" "tools-x68k/aiq/README.TXT" "tools-x68k/fntadr/README.TXT"
        "LICENSES/ymfm-BSD-3-Clause.txt"
        "LICENSES/SoftFloat-BSD-3-Clause.txt"
        "LICENSES/XEiJ-License.txt"
        "LICENSES/Sharp-Products-Users-Forum-License.txt"
        "ix68k.cfg.example" "keyboard.cfg.example" "README.md"
        ".mcp.json" "tools-host/mcp/ix68k_mcp_server.py" "tools-host/mcp/ix68k_wire.py" "tools-host/mcp/pyproject.toml"
        "tools-host/mcp/uv.lock" "tools-host/mcp/MCP_TOOLS.md" "tools-host/mcp/text_profiles" "tools-host/mcp/README.md"
        "tools-host/hu68disk/hu68disk" "tools-host/hu68disk/README.md" "tools-host/sram_switch/sram_switch"
        "tools-host/sram_switch/README.md" "tools-host/aiq-bridge/aiq_bridge.py" "tools-host/aiq-bridge/docs_search.py"
        "tools-host/aiq-bridge/prompts/system.md" "tools-host/aiq-bridge/README.md"
        "tools-host/cgrom_gen/cgrom_gen" "tools-host/cgrom_gen/README.md"
        "rom" "disk" "docs/x68k-usage"
    )
    local path
    for path in "${required[@]}"; do
        [[ -e "$pkg/$path" ]] || die "ZIPに必要なパスが無い: $path"
    done

    local empty_dir
    [[ -z "$(ls -A "$pkg/rom")" ]] || die "rom/ が空でない"
    [[ "$(ls -A "$pkg/disk")" == "BLANK.XDF" ]] || die "disk/ が BLANK.XDF だけでない"
    cmp -s "$BLANK_XDF" "$pkg/disk/BLANK.XDF" || die "disk/BLANK.XDF が雛形と一致しない"
    if unzip -Z1 "$zip_path" | grep -Ev "$BLANK_XDF_ZIP_REGEX" | grep -Eiq "$FORBIDDEN_EXT_REGEX"; then
        unzip -Z1 "$zip_path" | grep -Ev "$BLANK_XDF_ZIP_REGEX" | grep -Ei "$FORBIDDEN_EXT_REGEX"
        die "ROM・メディアらしきファイルがZIPに含まれている"
    fi
    if unzip -Z1 "$zip_path" | grep -q '^__MACOSX/'; then
        die "ZIPに __MACOSX が混入している"
    fi

    if is_linux; then
        verify_linux_binary "$pkg"
    else
        local exe="$pkg/ix68k.app/Contents/MacOS/ix68k"
        if otool -L "$exe" | grep -qi 'homebrew'; then
            die "ix68kがHomebrewのライブラリを参照している"
        fi
        otool -L "$exe" | grep -q '@rpath/libSDL3' || die "SDL3が@rpath参照になっていない"
        codesign --verify --deep --strict "$pkg/ix68k.app" || die "署名の検証に失敗"
    fi
    local main_exe="$pkg/ix68k"
    is_linux || main_exe="$pkg/ix68k.app/Contents/MacOS/ix68k"
    if strings -a "$main_exe" | grep -qF "$ROOT"; then
        die "ix68kにビルド環境のパス（$ROOT）が埋め込まれている"
    fi
    "$pkg/$TOOLS_HOST_DIR/ix68k-hostfile/ix68k-hostfile" --help > /dev/null || die "ix68k-hostfile --help が失敗"
    "$pkg/$TOOLS_HOST_DIR/cgrom_gen/cgrom_gen" --version > /dev/null || die "cgrom_gen --version が失敗"
    # hu68disk: 空イメージ作成→ファイル追加→取り出しの往復で動作を確認する
    local hu="$pkg/$TOOLS_HOST_DIR/hu68disk/hu68disk" hu_tmp="$check_dir/hu68disk_smoke"
    mkdir -p "$hu_tmp"
    "$hu" mkfs "$hu_tmp/t.xdf" --force > /dev/null || die "hu68disk mkfs が失敗"
    printf 'hu68disk smoke
' > "$hu_tmp/in.x"
    "$hu" copy "$hu_tmp/t.xdf" "$hu_tmp/in.x" / > /dev/null || die "hu68disk copy が失敗"
    "$hu" extract "$hu_tmp/t.xdf" /in.x "$hu_tmp/out.x" > /dev/null || die "hu68disk extract が失敗"
    cmp "$hu_tmp/in.x" "$hu_tmp/out.x" || die "hu68disk の往復でデータが一致しない"
    log "検証OK: $zip_path"
}

main() {
    local name stage_root install_dir zip_path
    name="$(detect_package_name)"
    stage_root="$OUT_DIR/stage"
    install_dir="$OUT_DIR/install"
    zip_path="$OUT_DIR/$name-$VERSION.zip"

    select_compiler
    rm -rf "$stage_root" "$install_dir" "$PYI_DIR" "$BUILD_DIR"
    mkdir -p "$stage_root/$name"

    build_and_install "$install_dir"
    check_hostdrv
    assemble_package "$stage_root/$name" "$install_dir"
    make_zip "$stage_root" "$name" "$zip_path"
    verify_zip "$zip_path" "$name"
    ls -lh "$zip_path"
}

main
