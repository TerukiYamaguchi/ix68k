cmake_minimum_required(VERSION 3.16)
project(ix68k C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# MSVCはソースをBOM無しUTF-8だとシステムコードページ(日本語版=932)で誤解釈し、
# コメント中のマルチバイト列を構文の一部として誤認する(C4819)。/utf-8で回避。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# リリースビルド用の PGO（docs/ix68k-internals/design/pgo_design.md）。
# X68K_PGO_GENERATE=ON で計装ビルド、X68K_PGO_PROFILE=<.profdata> で適用ビルド。
# 指定したのに PGO が効かない配布物を作らないよう、不正な指定は configure で止める。
option(X68K_PGO_GENERATE "Build with Clang PGO instrumentation" OFF)
set(X68K_PGO_PROFILE "" CACHE FILEPATH "Clang .profdata applied to the build")

# 通常ビルドでも、リポジトリ同梱のプロファイル（packaging/release/pgo/）があれば適用する。
# 暗黙の適用なので、Clang 以外・Debug 系・プロファイル無しのときは黙って PGO なしにする。
# 同梱プロファイルは採取時のソースとずれうるため、不一致の警告は出さない
# （stage.sh は X68K_PGO_PROFILE を明示するので、警告は出て件数が数えられる）。
option(X68K_PGO_USE_BUNDLED "Apply the bundled PGO profile when available (Clang Release builds)" ON)
set(X68K_PGO_BUNDLED_APPLIED OFF)
if(X68K_PGO_USE_BUNDLED AND NOT X68K_PGO_GENERATE AND NOT X68K_PGO_PROFILE
   AND CMAKE_C_COMPILER_ID MATCHES "^(Clang|AppleClang)$"
   AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    # パッケージ名は stage.sh の detect_package_name と同じ規則
    set(X68K_PGO_PACKAGE "")
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(X68K_PGO_PACKAGE "ix68k-macos-arm64")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(X68K_PGO_PACKAGE "ix68k-macos-x64")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(X68K_PGO_PACKAGE "ix68k-linux-x64")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
        # clang-cl（Clang 判定）のときだけ入る。MSVC は上の Clang 判定で外れる
        set(X68K_PGO_PACKAGE "ix68k-windows-x64")
    endif()
    set(X68K_PGO_BUNDLED "${CMAKE_CURRENT_SOURCE_DIR}/packaging/release/pgo/${X68K_PGO_PACKAGE}.profdata")
    if(X68K_PGO_PACKAGE AND EXISTS "${X68K_PGO_BUNDLED}")
        message(STATUS "PGO: applying bundled profile ${X68K_PGO_BUNDLED}")
        set(X68K_PGO_PROFILE "${X68K_PGO_BUNDLED}")
        set(X68K_PGO_BUNDLED_APPLIED ON)
    endif()
endif()

# hu68disk: Human68kディスクイメージのファイル操作CLI。sjis.cはix68k本体と共有。
# Release ZIPでは tools-host/hu68disk/ へ置く。macOS/Linuxはstage.shがMakefileでビルドし、
# Windowsはstage.ps1がこのターゲットのhu68disk.exeを使う。
# PGOはix68k本体とCPUコア(mc68000)だけに適用する。add_compile_options()は以降に作る
# ターゲットへ効くため、hu68diskは下のPGO設定より前に定義して対象から外す。
add_executable(hu68disk
    tools-host/hu68disk/main.c tools-host/hu68disk/cmd.c tools-host/hu68disk/dir.c
    tools-host/hu68disk/fat.c tools-host/hu68disk/fsck.c tools-host/hu68disk/image.c
    tools-host/hu68disk/compat.c src/peripheral/sjis.c)
target_include_directories(hu68disk PRIVATE tools-host/hu68disk src/peripheral)
if(MSVC)
    target_compile_definitions(hu68disk PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# sram_switch: sram.datのオフライン設定CLI。Windowsのリリースzipにも入れる。
add_executable(sram_switch tools-host/sram_switch/sram_switch.c)
if(MSVC)
    target_compile_definitions(sram_switch PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(sram_switch PRIVATE /utf-8)
endif()

# cgrom_gen: ビットマップフォント(BDF)から互換CGROMを作るCLI。Windowsのリリースzipにも入れる。
add_executable(cgrom_gen tools-host/cgrom_gen/cgrom_gen.c)
if(MSVC)
    target_compile_definitions(cgrom_gen PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cgrom_gen PRIVATE /utf-8)
endif()

if(X68K_PGO_GENERATE OR X68K_PGO_PROFILE)
    if(X68K_PGO_GENERATE AND X68K_PGO_PROFILE)
        message(FATAL_ERROR "X68K_PGO_GENERATE and X68K_PGO_PROFILE are mutually exclusive")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
        message(FATAL_ERROR "PGO requires Clang (compiler: ${CMAKE_C_COMPILER_ID})")
    endif()
    # --- 他コンパイラで PGO を使う場合（未実装・未検証） ---
    # PGO のプロファイル形式はコンパイラごとに別で互換がない。clang の .profdata は
    # GCC・MSVC では読めないため、そのコンパイラで採り直す必要がある。
    # tools-host/pgo/train.py と stage.sh は llvm-profdata 前提なので、あわせて直すこと。
    #
    # [GCC] 上の if(NOT ... Clang) を消すか GNU を許可し、下の if(X68K_PGO_GENERATE) 節を
    #       次のように置き換える。プロファイルは .gcda（オブジェクトのパスと
    #       コンパイラの版に依存）で、X68K_PGO_PROFILE にはディレクトリを渡す。
    #   計装:   add_compile_options(-fprofile-generate=${X68K_PGO_DIR})
    #           add_link_options(-fprofile-generate)
    #   適用:   add_compile_options(-fprofile-use=${X68K_PGO_DIR} -fprofile-correction
    #                               -Wno-missing-profile)
    #
    # [MSVC] /GL と /LTCG が必須（LTCG は過去の測定で悪化した前例あり。要再測定）。
    #   計装:   add_compile_options(/GL)
    #           add_link_options(/LTCG:PGI)
    #   集約:   pgomgr で .pgd にまとめる
    #   適用:   add_link_options(/LTCG:PGO)（.pgd をリンク時に参照）
    #   SIGTERM が使えないため、計装ビルドの終了経路が別に要る。
    #
    # clang-cl は Clang 判定を通るので、このブロックの変更は不要（docs/ix68k-internals/design/pgo_design.md 参照）。
    # 適用範囲はix68k本体とCPUコア(mc68000)だけ。ここではディレクトリスコープで追加し
    # （add_subdirectory(mc68000)が継承する）、add_subdirectoryの直後に外してix68kへだけ付け直す。
    # テスト・hu68disk等は配布物に入らず、定義も違うため対象外。
    set(X68K_PGO_COMPILE_OPTIONS "")
    set(X68K_PGO_LINK_OPTIONS "")
    set(X68K_PGO_DEFINITIONS "")
    if(X68K_PGO_GENERATE)
        set(X68K_PGO_COMPILE_OPTIONS -fprofile-instr-generate)
        set(X68K_PGO_LINK_OPTIONS -fprofile-instr-generate)
        set(X68K_PGO_DEFINITIONS X68K_PGO_GENERATE=1)
    else()
        if(NOT EXISTS "${X68K_PGO_PROFILE}")
            message(FATAL_ERROR "X68K_PGO_PROFILE not found: ${X68K_PGO_PROFILE}")
        endif()
        # profdata に載らない外部ライブラリ(softfloat 等)の警告だけ抑える。
        # 不一致(profile-instr-out-of-date)は警告として出す。
        set(X68K_PGO_COMPILE_OPTIONS -fprofile-instr-use=${X68K_PGO_PROFILE} -Wno-profile-instr-unprofiled)
        if(X68K_PGO_BUNDLED_APPLIED)
            list(APPEND X68K_PGO_COMPILE_OPTIONS -Wno-profile-instr-out-of-date)
        endif()
    endif()
    add_compile_options(${X68K_PGO_COMPILE_OPTIONS})
    if(X68K_PGO_LINK_OPTIONS)
        add_link_options(${X68K_PGO_LINK_OPTIONS})
    endif()
    if(X68K_PGO_DEFINITIONS)
        add_compile_definitions(${X68K_PGO_DEFINITIONS})
    endif()
endif()

# mc68000 ライブラリ（git submodule）
add_subdirectory(extern/cmc68000)

# mc68000へ継承させたPGOのフラグを、以降のターゲット（テスト等）から外す。
# ix68k本体へはtarget_*で付け直す（下のtarget_include_directories(ix68k ...)の直後）。
if(X68K_PGO_COMPILE_OPTIONS OR X68K_PGO_LINK_OPTIONS OR X68K_PGO_DEFINITIONS)
    foreach(prop COMPILE_OPTIONS LINK_OPTIONS COMPILE_DEFINITIONS)
        get_directory_property(dir_vals ${prop})
        if(prop STREQUAL "COMPILE_OPTIONS")
            set(pgo_vals ${X68K_PGO_COMPILE_OPTIONS})
        elseif(prop STREQUAL "LINK_OPTIONS")
            set(pgo_vals ${X68K_PGO_LINK_OPTIONS})
        else()
            set(pgo_vals ${X68K_PGO_DEFINITIONS})
        endif()
        foreach(v IN LISTS pgo_vals)
            list(REMOVE_ITEM dir_vals "${v}")
        endforeach()
        set_directory_properties(PROPERTIES ${prop} "${dir_vals}")
    endforeach()
endif()

# Windows専用POSIX互換シム。x68k_bus.c/hostfile.c/hostdrv_remote.cが
# 使うdirent/mkstemp/statvfs等の実体をwin_compat.cが提供するため、これらをリンクする
# 全ターゲットに追加する（WIN32以外では空文字列＝無視）。
set(WIN_COMPAT_SRC "")
if(WIN32)
    set(WIN_COMPAT_SRC src/platform/win_compat.c)
endif()

# SDL3専用（SDL2サポートは廃止。PLATFORM_NULL=1 で強制ヘッドレス）。
# ネイティブファイルダイアログ(SDL_ShowOpenFileDialog等、ディスク選択ウィンドウ
# ディスク選択ウィンドウで使用)がSDL3のみのAPIのため、SDL2への後方互換を維持する意味が薄い。
option(PLATFORM_NULL "Force headless build (platform_null)" OFF)

if(PLATFORM_NULL)
    set(PLATFORM_SRC src/platform/platform_null.c)
    set(PLATFORM_LIBS "")
    message(STATUS "Using platform_null (headless)")
else()
    find_package(SDL3 CONFIG REQUIRED)
    set(PLATFORM_SRC src/platform/platform_sdl3.c src/platform/platform_sdl3_audio.c
                     src/platform/platform_sdl3_input.c src/platform/platform_sdl3_aux_window.c)
    set(PLATFORM_LIBS SDL3::SDL3)
    message(STATUS "Using platform_sdl3")
endif()

# 外部デバッガ ix68k-debugger をメニューから起動する（開発ツリー向け）。
# POSIX（macOS/Linux）は fork+exec、Windows は CreateProcess で同じ契約を実装する。
if(NOT PLATFORM_NULL)
    if(WIN32)
        list(APPEND PLATFORM_SRC src/platform/debugger_launcher_win.c)
    else()
        list(APPEND PLATFORM_SRC src/platform/debugger_launcher.c)
    endif()
endif()

# ホスト側補助ウィンドウ: macOSのみCocoa APIでフルスクリーン
# 中のSpace切り替え問題に対処する(NSWindowCollectionBehaviorFullScreenAuxiliary)。
if(APPLE AND NOT PLATFORM_NULL)
    enable_language(OBJC)
    list(APPEND PLATFORM_SRC src/platform/platform_macos_aux.m)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    list(APPEND PLATFORM_LIBS ${COCOA_LIBRARY})
endif()

# File/Window SizeのネイティブメニューバーをWin32のHMENUで再現する
# （macOS版のCocoaメニューバーに相当。SDL3自体はクロスプラットフォームAPIの
# ためメニューバーを持たない）。
if(WIN32 AND NOT PLATFORM_NULL)
    enable_language(RC)  # ix68k.exe へアイコンを埋め込むため（packaging/windows/ix68k.rc.in）
    list(APPEND PLATFORM_SRC src/platform/platform_win_aux.c)
endif()

# File/Window SizeメニューをSDL自前描画で再現する（Linux等、
# macOS/Windowsのようなネイティブメニューバー/HMENUに相当するAPIが無い
# デスクトップ向け）。
if(NOT APPLE AND NOT WIN32 AND NOT PLATFORM_NULL)
    list(APPEND PLATFORM_SRC src/platform/platform_linux_aux.c)
endif()

# src/peripheral/sjis.c（Shift-JIS⇔UTF-8変換）が使う iconv。macOSは別ライブラリ、
# Linux(glibc)はlibc内蔵のため見つからなくてもよい（tools-host/hu68disk/Makefileと同じ扱い）。
find_library(ICONV_LIBRARY iconv)
if(NOT ICONV_LIBRARY)
    set(ICONV_LIBRARY "")
endif()

# hu68diskのリンク（iconv）。ターゲット自体は上のPGO設定より前で定義している。
target_link_libraries(hu68disk PRIVATE ${ICONV_LIBRARY})
# tests/audio_mixer_test.cが使うsin()等のlibm。macOS/MSVCはCRTに内蔵で暗黙リンクされる
# ため気づかれていなかったが、Linux(glibc)は-lmを明示しないとリンクエラーになる
# （調査で判明）。
find_library(MATH_LIBRARY m)
if(NOT MATH_LIBRARY)
    set(MATH_LIBRARY "")
endif()

# バージョン文字列（x68k_version.h）。リリースは X68K_APP_VERSION（日付）、日々のビルドは
# git describe。コミットが進んでも再configureなしで追従するよう、ビルドのたびに生成する。
set(X68K_VERSION_DIR ${CMAKE_BINARY_DIR}/generated)
if(DEFINED X68K_RELEASE_VERSION)
    set(X68K_VERSION_RELEASE_ARG ${X68K_RELEASE_VERSION})
else()
    set(X68K_VERSION_RELEASE_ARG "")
endif()
add_custom_target(x68k_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUTPUT=${X68K_VERSION_DIR}/x68k_version.h
            -DRELEASE_VERSION=${X68K_VERSION_RELEASE_ARG}
            -P ${CMAKE_SOURCE_DIR}/cmake/gen_version.cmake
    BYPRODUCTS ${X68K_VERSION_DIR}/x68k_version.h
    COMMENT "x68k_version.h を生成")
# PLATFORM_SRC（メニューが版数を表示する）を共有するテストにも版数ヘッダを使わせる。
add_executable(ix68k
    src/main.c
    src/app/config.c
    src/app/fpu_config.c
    src/app/keyboard_config.c
    src/app/rom_check.c
    src/app/runtime.c
    src/app/checkpoint.c
    src/ui/disk_window.c
    src/ui/keyboard_window.c
    src/mcp/server.c
    src/mcp/sock_path.c
    src/mcp/port.c
    src/mcp/screenshot.c
    src/mcp/text_event.c
    src/mcp/text_probe.c
    src/mcp/commands.c
    src/mcp/debugger.c
    src/mcp/perf.c
    src/mcp/dbg_cond.c
    src/iocs.c
    src/bus/x68k_bus.c
    src/bus/snapshot.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sha256.c
    src/peripheral/media_fp.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    src/render/gvram_renderer.c
    src/render/tvram_renderer.c
    src/render/sprite_renderer.c
    src/render/video_effects.c
    src/render/compose.c
    src/audio/audio_mixer.c
    src/audio/ym2151.c
    src/platform/aux_font.c
    ${PLATFORM_SRC}
    ${WIN_COMPAT_SRC}
)

# ヘッドレス版はSDL3版と実行ファイル名で区別する（同じbuild/配下に置くため、
# 誤って古いSDL3版を実行してしまう取り違え事故を防ぐ）。
if(PLATFORM_NULL)
    set_target_properties(ix68k PROPERTIES OUTPUT_NAME ix68k-nogui)
    target_compile_definitions(ix68k PRIVATE X68K_HEADLESS=1)
endif()

# X68K_RELEASE_LAYOUT=ON のときは Release ZIP 用に、実行ファイルを展開先トップへ置き
# SDL3共有ライブラリを lib/ へ同梱する（実行ファイルは $ORIGIN/lib を参照）。
option(X68K_RELEASE_LAYOUT "Install a relocatable Release ZIP layout (Linux)" OFF)
if(UNIX AND NOT APPLE AND NOT PLATFORM_NULL AND X68K_RELEASE_LAYOUT)
    set_target_properties(ix68k PROPERTIES INSTALL_RPATH "$ORIGIN/lib")
    install(TARGETS ix68k RUNTIME DESTINATION .)
    install(FILES extern/SDL_GameControllerDB/gamecontrollerdb.txt DESTINATION .)
    # 実行時にローダが探すのは soname の libSDL3.so.0 だけなので、その実体を通常ファイルとして
    # 1つだけ置く（シンボリックリンクを入れない。ZIPの展開ツールがリンクを扱えなくても壊れない）。
    install(CODE "
        get_filename_component(sdl3_real \"$<TARGET_FILE:SDL3::SDL3>\" REALPATH)
        get_filename_component(sdl3_dir \"\${sdl3_real}\" DIRECTORY)
        if(NOT EXISTS \"\${sdl3_dir}/libSDL3.so.0\")
            message(FATAL_ERROR \"共有版 libSDL3.so.0 が見つからない（静的SDL3ではRelease ZIPを作れない）: \${sdl3_dir}\")
        endif()
        get_filename_component(sdl3_so \"\${sdl3_dir}/libSDL3.so.0\" REALPATH)
        file(INSTALL \"\${sdl3_so}\" DESTINATION \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/lib\"
             RENAME libSDL3.so.0)
    ")
elseif(UNIX AND NOT APPLE AND NOT PLATFORM_NULL)
    # Linux向けデスクトップ統合（アプリケーションメニューとアイコン）。
    # アイコンは packaging/icons/linux/ の生成済みファイルをhicolorテーマへそのまま配置する。
    include(GNUInstallDirs)
    # .desktop の Exec=ix68k が PATH 上で解決できるよう本体も配置する。
    install(TARGETS ix68k RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/ix68k.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(DIRECTORY packaging/icons/linux/hicolor
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
endif()

# macOS向けix68k.appバンドル。リリース時（cmake --install）にだけ組み立て、開発ビルドの
# build/ix68k のパスは変えない（docs/toolsの多数が参照するため）。署名は別作業。
if(APPLE AND NOT PLATFORM_NULL)
    set(X68K_APP_VERSION "0.0.0" CACHE STRING "CFBundleVersion / CFBundleShortVersionString")
    set(X68K_BUNDLE_IDENTIFIER "com.github.terukiyamaguchi.ix68k" CACHE STRING "CFBundleIdentifier")
    configure_file(packaging/macos/Info.plist.in ${CMAKE_BINARY_DIR}/packaging/macos/Info.plist @ONLY)
    # install(TARGETS)はビルド時のRPATH（Homebrewの/opt/homebrew/lib）をINSTALL_RPATHに置換する。
    # 残すとdyldがHomebrew版SDL3を先に見つけ、同梱版が使われない。
    set_target_properties(ix68k PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
    install(TARGETS ix68k RUNTIME DESTINATION ix68k.app/Contents/MacOS)
    install(FILES ${CMAKE_BINARY_DIR}/packaging/macos/Info.plist DESTINATION ix68k.app/Contents)
    install(FILES packaging/icons/macos/AppIcon.icns DESTINATION ix68k.app/Contents/Resources)
    # .appではSDL_GetBasePath()がContents/Resources/を返すため、実行ファイルの隣ではなくここへ置く。
    install(FILES extern/SDL_GameControllerDB/gamecontrollerdb.txt
            DESTINATION ix68k.app/Contents/Resources)
    # SDL3 dylibを同梱し、実行ファイルの参照を@rpath経由に書き換える（SDL3の無いMacでも起動させる）。
    # install名の書き換えで署名が無効になるため、最後にad-hoc署名し直す（arm64は無署名だと起動しない）。
    install(CODE "
        set(app_dir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/ix68k.app/Contents\")
        set(sdl3_src \"$<TARGET_FILE:SDL3::SDL3>\")
        get_filename_component(sdl3_real \"\${sdl3_src}\" REALPATH)
        get_filename_component(sdl3_name \"\${sdl3_real}\" NAME)
        file(INSTALL \"\${sdl3_real}\" DESTINATION \"\${app_dir}/Frameworks\")
        execute_process(COMMAND otool -D \"\${sdl3_real}\" OUTPUT_VARIABLE sdl3_id_out COMMAND_ERROR_IS_FATAL ANY)
        string(REPLACE \"\\n\" \";\" sdl3_id_lines \"\${sdl3_id_out}\")
        list(GET sdl3_id_lines 1 sdl3_id)
        string(STRIP \"\${sdl3_id}\" sdl3_id)
        execute_process(COMMAND install_name_tool -id \"@rpath/\${sdl3_name}\" \"\${app_dir}/Frameworks/\${sdl3_name}\" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND install_name_tool -change \"\${sdl3_id}\" \"@rpath/\${sdl3_name}\"
                                \"\${app_dir}/MacOS/ix68k\" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --force --sign - \"\${app_dir}/Frameworks/\${sdl3_name}\" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --force --sign - \"\${app_dir}/MacOS/ix68k\" COMMAND_ERROR_IS_FATAL ANY)
    ")
endif()

# Windows向け: エクスプローラー・タスクバーに表示する実行ファイルのアイコンを埋め込む。
if(WIN32 AND NOT PLATFORM_NULL)
    set(X68K_APP_ICON_PATH ${CMAKE_SOURCE_DIR}/packaging/icons/windows/app.ico)
    set(X68K_APP_RC ${CMAKE_BINARY_DIR}/packaging/windows/ix68k.rc)
    configure_file(packaging/windows/ix68k.rc.in ${X68K_APP_RC} @ONLY)
    target_sources(ix68k PRIVATE ${X68K_APP_RC})
    # アイコンを差し替えたとき再リンクされるようにする。
    set_source_files_properties(${X68K_APP_RC} PROPERTIES OBJECT_DEPENDS ${X68K_APP_ICON_PATH})
    # Release ZIP: 実行ファイル・gamecontrollerdb.txt・SDL3.dll を展開先トップへ並べる。
    install(TARGETS ix68k RUNTIME DESTINATION .)
    install(FILES extern/SDL_GameControllerDB/gamecontrollerdb.txt DESTINATION .)
    if(CMAKE_VERSION VERSION_LESS 3.21)
        message(FATAL_ERROR "Windows の install には CMake 3.21 以上が必要（TARGET_RUNTIME_DLLS）")
    endif()
    install(FILES $<TARGET_RUNTIME_DLLS:ix68k> DESTINATION .)
endif()

add_dependencies(ix68k x68k_version)
target_include_directories(ix68k PRIVATE src include ${X68K_VERSION_DIR})
if(X68K_PGO_COMPILE_OPTIONS)
    target_compile_options(ix68k PRIVATE ${X68K_PGO_COMPILE_OPTIONS})
endif()
if(X68K_PGO_LINK_OPTIONS)
    target_link_options(ix68k PRIVATE ${X68K_PGO_LINK_OPTIONS})
endif()
if(X68K_PGO_DEFINITIONS)
    target_compile_definitions(ix68k PRIVATE ${X68K_PGO_DEFINITIONS})
endif()
target_link_libraries(ix68k PRIVATE mc68000 ${PLATFORM_LIBS} ${ICONV_LIBRARY})
if(WIN32)
    # src/mcp/server.c のWinsock化（AF_UNIX/afunix.h、AF_INET TCP）向け
    target_link_libraries(ix68k PRIVATE ws2_32)
    # WSAPoll()・WSAPOLLFD・POLLOUT と afunix.h は winsock2.h の
    # _WIN32_WINNT >= 0x0600 ガード内にしか無い。未定義時の既定値は
    # ツールチェーン依存（mingw-w64は古い系列だと0x0502）なので明示する。
    target_compile_definitions(ix68k PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
# 開発ビルド（.app外）でDockアイコンを設定するための icns パスを埋め込む。ビルド環境の
# 絶対パスがバイナリに入るため、配布ビルド（packaging/release/stage.sh）はOFFにする。
option(X68K_DEV_DOCK_ICON "開発ビルドでDockアイコンのicnsパスを埋め込む（macOS）" ON)
if(APPLE AND NOT PLATFORM_NULL AND X68K_DEV_DOCK_ICON)
    target_compile_definitions(ix68k PRIVATE
        X68K_DEV_ICON_PATH="${CMAKE_SOURCE_DIR}/packaging/icons/macos/AppIcon.icns")
endif()
if(MSVC)
    target_compile_options(ix68k PRIVATE /W4)
else()
    target_compile_options(ix68k PRIVATE -Wall -Wextra)
endif()

# 未知パッド対応: SDL3内蔵のゲームパッド定義DBに無い
# デバイスはSDL_Gamepadとして認識されないため、コミュニティDB（mdqinc/
# SDL_GameControllerDB、extern/にsubmodule）を実行ファイルと同じディレクトリへ
# 配置し、pal_init()でSDL_AddGamepadMappingsFromFile()により読み込む。
if(NOT PLATFORM_NULL)
    add_custom_command(TARGET ix68k POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/extern/SDL_GameControllerDB/gamecontrollerdb.txt
            $<TARGET_FILE_DIR:ix68k>/gamecontrollerdb.txt
        COMMENT "Copying gamecontrollerdb.txt next to ix68k executable"
    )
endif()

enable_testing()

# テストの検査・セットアップにはassert式内の副作用も含まれるため、Release系でも
# NDEBUGを解除して実行する。そうしないと検査本体を省略したまま成功表示だけが
# 残る。以降に定義する全テストターゲットへディレクトリスコープで
# 自動適用し、手書きの対象リストで将来の追加分が漏れないようにする（本体
# ix68kはこれより前で定義済み、mc68000はadd_subdirectoryで別スコープのため
# いずれも対象外）。
if(MSVC)
    add_compile_options(/UNDEBUG)
else()
    add_compile_options(-UNDEBUG)
endif()
# テストにはPGOのフラグを付けない（配布する ix68k と別の定義でコンパイルされ、
# プロファイルと関数の形が合わないため。mc68000の直後で外している）。

add_executable(config_test
    tests/config_test.c
    src/app/config.c
    src/app/keyboard_config.c
)
target_include_directories(config_test PRIVATE src include)
target_compile_definitions(config_test PRIVATE
    IX68K_CFG_EXAMPLE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/profiles/ix68k.cfg.example")
target_link_libraries(config_test PRIVATE mc68000)
add_test(NAME config_test COMMAND config_test)

# IPLROM 起動前検証（誤った ROM / series / CPU の組合せ）。
add_executable(rom_check_test
    tests/rom_check_test.c
    src/app/rom_check.c
)
target_include_directories(rom_check_test PRIVATE src include)
target_link_libraries(rom_check_test PRIVATE mc68000)
add_test(NAME rom_check_test COMMAND rom_check_test)

# X68030 の CPU 所有 FPCP（MC68882）構成選択。
add_executable(fpu_fpcp_x68030_test
    tests/fpu_fpcp_x68030_test.c
    src/app/fpu_config.c
)
target_include_directories(fpu_fpcp_x68030_test PRIVATE src include)
target_link_libraries(fpu_fpcp_x68030_test PRIVATE mc68000)
add_test(NAME fpu_fpcp_x68030_test COMMAND fpu_fpcp_x68030_test)

# CPUクロックを60Hz論理フレームへ端数なく配分する。
add_executable(frame_timing_test tests/frame_timing_test.c)
target_include_directories(frame_timing_test PRIVATE src include)
add_test(NAME frame_timing_test COMMAND frame_timing_test)

# MCPソケットパスの環境変数解決ロジック。純ロジックのため
# ソケット・SDL・busいずれにも依存しない。
add_executable(mcp_sock_path_test tests/mcp_sock_path_test.c src/mcp/sock_path.c)
target_include_directories(mcp_sock_path_test PRIVATE src include)
add_test(NAME mcp_sock_path_test COMMAND mcp_sock_path_test)

# MCP TCPポートの環境変数解決ロジック。Windows専用機能だが
# ロジックはプラットフォーム非依存に書いてあるので macOS/Linux でもテストできる。
add_executable(mcp_port_test tests/mcp_port_test.c src/mcp/port.c)
target_include_directories(mcp_port_test PRIVATE src include)
add_test(NAME mcp_port_test COMMAND mcp_port_test)

# Windows専用の実ソケット試験。実際に bind/listen/connect するため
# プラットフォーム非依存にはできず、Windowsビルドでのみ定義する。src/mcp/server.c は
# リンクしない（pal_log・SDL・busを引き込むため）。検証したいのはソケットオプションと
# アドレス指定の実挙動なので、server.c のWindows版 mcp_open_listener() と同じ手順を
# テスト内で再現する（本体と試験でコードが二重になる弱点はあるが、
# mcp_open_listener() が static かつ pal_log 依存でテストから素で呼べないため）。
if(WIN32)
    add_executable(mcp_port_socket_test tests/mcp_port_socket_test.c src/mcp/port.c)
    target_include_directories(mcp_port_socket_test PRIVATE src include)
    target_link_libraries(mcp_port_socket_test PRIVATE ws2_32)
    target_compile_definitions(mcp_port_socket_test PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    add_test(NAME mcp_port_socket_test COMMAND mcp_port_socket_test)
endif()

# MCP PASTEの制御文字→Ctrl+英字キー注入を、SDL非依存のplatform_nullで検証する。
add_executable(platform_key_inject_test
    tests/platform_key_inject_test.c
    src/platform/platform_null.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(platform_key_inject_test PRIVATE src include)
add_test(NAME platform_key_inject_test COMMAND platform_key_inject_test)

# SDL PAUSE キー → NMI 要求の経路検証。
# platform_sdl3固有の経路のため、platform_nullではビルドしない。
if(NOT PLATFORM_NULL)
    add_executable(test_nmi_pause
        tests/test_nmi_pause.c
        src/platform/aux_font.c
        ${PLATFORM_SRC}
        ${WIN_COMPAT_SRC}
    )
    target_include_directories(test_nmi_pause PRIVATE src include ${X68K_VERSION_DIR})
    add_dependencies(test_nmi_pause x68k_version)
    target_link_libraries(test_nmi_pause PRIVATE ${PLATFORM_LIBS})
    add_test(NAME test_nmi_pause COMMAND test_nmi_pause)

    # F12/Shift+F12 → 補助ウィンドウ起動要求の経路検証。
    # test_nmi_pauseと同じSDL_PushEvent合成注入・ヘッドレス手法。
    add_executable(test_aux_hotkey
        tests/test_aux_hotkey.c
        src/platform/aux_font.c
        ${PLATFORM_SRC}
        ${WIN_COMPAT_SRC}
    )
    target_include_directories(test_aux_hotkey PRIVATE src include ${X68K_VERSION_DIR})
    add_dependencies(test_aux_hotkey x68k_version)
    target_link_libraries(test_aux_hotkey PRIVATE ${PLATFORM_LIBS})
    add_test(NAME test_aux_hotkey COMMAND test_aux_hotkey)

    # 出力キューが枯渇した後は、即時再生を継続せず事前バッファリングから
    # 回復する。SDL dummy audioで物理デバイスなしに検証する。
    add_executable(test_audio_recovery
        tests/test_audio_recovery.c
        src/platform/aux_font.c
        ${PLATFORM_SRC}
        ${WIN_COMPAT_SRC}
    )
    target_include_directories(test_audio_recovery PRIVATE src include ${X68K_VERSION_DIR})
    add_dependencies(test_audio_recovery x68k_version)
    target_link_libraries(test_audio_recovery PRIVATE ${PLATFORM_LIBS})
    add_test(NAME test_audio_recovery COMMAND test_audio_recovery)
endif()

# ADPCM(MSM6258)コーデック単体テスト。SDL2/DMAC/バス層に
# 依存しない純ロジックのため無条件でビルドする。
add_executable(adpcm_codec_test
    tests/adpcm_codec_test.c
    src/peripheral/adpcm_codec.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(adpcm_codec_test PRIVATE src include)
target_compile_definitions(adpcm_codec_test PRIVATE
    X68K_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    X68K_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/adpcm_test_output"
)
add_test(NAME adpcm_codec_test COMMAND adpcm_codec_test)

# メニューからの ix68k-debugger 起動（POSIXのみ）。ダミーの起動スクリプトで
# 起動・二重起動の抑止・終了後の再起動・スクリプト欠如を確認する。
if(NOT WIN32)
    add_executable(debugger_launcher_test
        tests/debugger_launcher_test.c
        src/platform/debugger_launcher.c
    )
    target_include_directories(debugger_launcher_test PRIVATE src include)
    add_test(NAME debugger_launcher_test COMMAND debugger_launcher_test)
endif()
# disk/otoko100_sound（gitignore対象のゲーム資産）未配置環境では終了コード77で
# SKIP扱いにする（テスト側の実装と対）。
set_tests_properties(adpcm_codec_test PROPERTIES SKIP_RETURN_CODE 77)

# CGROM ANKフォントのビット判定ロジック単体テスト。SDL/ROM非依存の
# 純ロジックのため無条件でビルドする。
add_executable(aux_font_test
    tests/aux_font_test.c
    src/platform/aux_font.c
)
target_include_directories(aux_font_test PRIVATE src include)
add_test(NAME aux_font_test COMMAND aux_font_test)

# ADPCM(MSM6258)レジスタ + PPIポートC + OPM $1B 単体テスト。
# bus/CPU非依存の純ロジックのため無条件でビルドする。
add_executable(adpcm_test
    tests/adpcm_test.c
    src/peripheral/adpcm.c
    src/peripheral/adpcm_codec.c
)
target_include_directories(adpcm_test PRIVATE src include)
add_test(NAME adpcm_test COMMAND adpcm_test)

# 音声リサンプル+パン合成の単体テスト。SDL/バス非依存の
# 純ロジックのため無条件でビルドする。
add_executable(audio_mixer_test
    tests/audio_mixer_test.c
    src/audio/audio_mixer.c
)
target_include_directories(audio_mixer_test PRIVATE src include)
target_link_libraries(audio_mixer_test PRIVATE ${MATH_LIBRARY})
add_test(NAME audio_mixer_test COMMAND audio_mixer_test)

# fdc.c RECALIBRATE の CT2(OPM $1B bit6)プローブモード分岐の単体テスト。
# dmac.c/CPU非依存のためスタブ(tests/fdc_probe_test.c内)でリンクを閉じる。
add_executable(fdc_probe_test
    tests/fdc_probe_test.c
    src/peripheral/fdc.c
    src/peripheral/adpcm.c
    src/peripheral/adpcm_codec.c
)
target_include_directories(fdc_probe_test PRIVATE src include)
target_link_libraries(fdc_probe_test PRIVATE mc68000)
add_test(NAME fdc_probe_test COMMAND fdc_probe_test)

# FDC SCAN EQUAL(0x11)の単体テスト（IOCS _B_VERIFY用）。
# dmac.c/CPU非依存のためスタブ(tests/fdc_scan_test.c内)でリンクを閉じる。
add_executable(fdc_scan_test
    tests/fdc_scan_test.c
    src/peripheral/fdc.c
    src/peripheral/adpcm.c
    src/peripheral/adpcm_codec.c
)
target_include_directories(fdc_scan_test PRIVATE src include)
target_link_libraries(fdc_scan_test PRIVATE mc68000)
add_test(NAME fdc_scan_test COMMAND fdc_scan_test)

# FDC WRITE/READ DELETED DATA(0x09/0x0C)の単体テスト（IOCS _B_WRITED/_B_READDL用）。
# dmac.c/CPU非依存のためスタブ(tests/fdc_deleted_test.c内)でリンクを閉じる。
add_executable(fdc_deleted_test
    tests/fdc_deleted_test.c
    src/peripheral/fdc.c
    src/peripheral/adpcm.c
    src/peripheral/adpcm_codec.c
)
target_include_directories(fdc_deleted_test PRIVATE src include)
target_link_libraries(fdc_deleted_test PRIVATE mc68000)
add_test(NAME fdc_deleted_test COMMAND fdc_deleted_test)

# FDC ライトプロテクト(WP)の単体テスト。
# dmac.c/CPU非依存のためスタブ(tests/fdc_wp_test.c内)でリンクを閉じる。
add_executable(fdc_wp_test
    tests/fdc_wp_test.c
    src/peripheral/fdc.c
    src/peripheral/adpcm.c
    src/peripheral/adpcm_codec.c
)
target_include_directories(fdc_wp_test PRIVATE src include)
target_link_libraries(fdc_wp_test PRIVATE mc68000)
add_test(NAME fdc_wp_test COMMAND fdc_wp_test)

# fdc.c 媒体プロファイルの単体テスト。dmac.c/CPU非依存のため
# スタブ(tests/fdc_profile_test.c内)でリンクを閉じる。
add_executable(fdc_profile_test
    tests/fdc_profile_test.c
    src/peripheral/fdc.c
    src/peripheral/adpcm.c
    src/peripheral/adpcm_codec.c
)
target_include_directories(fdc_profile_test PRIVATE src include)
target_link_libraries(fdc_profile_test PRIVATE mc68000)
add_test(NAME fdc_profile_test COMMAND fdc_profile_test)

# fdc.c 回転数・転送速度（3モード）と媒体プロファイルの照合の単体テスト。
add_executable(fdc_rate_test
    tests/fdc_rate_test.c
    src/peripheral/fdc.c
    src/peripheral/adpcm.c
    src/peripheral/adpcm_codec.c
)
target_include_directories(fdc_rate_test PRIVATE src include)
target_link_libraries(fdc_rate_test PRIVATE mc68000)
add_test(NAME fdc_rate_test COMMAND fdc_rate_test)

# fdc.c FORMAT TRACK(0x0D)の単体テスト。dmac.c非依存のためスタブで閉じる。
add_executable(fdc_format_test
    tests/fdc_format_test.c
    src/peripheral/fdc.c
    src/peripheral/adpcm.c
    src/peripheral/adpcm_codec.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(fdc_format_test PRIVATE src include)
target_link_libraries(fdc_format_test PRIVATE mc68000)
add_test(NAME fdc_format_test COMMAND fdc_format_test)

# HostDrv comcodハンドラの単体テスト。bus/CPU非依存のため単独でリンクする。
add_executable(hostdrv_remote_test
    tests/hostdrv_remote_test.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(hostdrv_remote_test PRIVATE src include)
target_link_libraries(hostdrv_remote_test PRIVATE ${ICONV_LIBRARY})
add_test(NAME hostdrv_remote_test COMMAND hostdrv_remote_test)

# ymfm由来FM音源テーブルの単体テスト。
# ヘッダのみ・バス/CPU非依存のため無条件でビルドする。
add_executable(ym2151_tables_test
    tests/ym2151_tables_test.c
)
target_include_directories(ym2151_tables_test PRIVATE src include)
add_test(NAME ym2151_tables_test COMMAND ym2151_tables_test)

# YM2151合成コアの単体テスト。バス/CPU非依存のため無条件でビルドする。
add_executable(ym2151_gen_test
    tests/ym2151_gen_test.c
    src/audio/ym2151.c
)
target_include_directories(ym2151_gen_test PRIVATE src include)
add_test(NAME ym2151_gen_test COMMAND ym2151_gen_test)

# OPM内蔵タイマA/Bの単体テスト。バス/CPU非依存のため無条件でビルドする。
add_executable(opm_timer_test
    tests/opm_timer_test.c
    src/audio/ym2151.c
    src/peripheral/opm.c
    src/peripheral/mfp.c
)
target_include_directories(opm_timer_test PRIVATE src include)
target_link_libraries(opm_timer_test PRIVATE mc68000)
add_test(NAME opm_timer_test COMMAND opm_timer_test)

# MFP GPIP3(FMIRQ)割り込み配線の単体テスト
# （opm_design.md §4）。bus_iack()を間接的に叩くため
# x68k_bus.c本体+全周辺デバイスをリンクする(ix68k実行体からmain.c/iocs.c/
# mcp・platform層を除いた構成)。
add_executable(mfp_fmirq_test
    tests/mfp_fmirq_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(mfp_fmirq_test PRIVATE src include)
target_link_libraries(mfp_fmirq_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME mfp_fmirq_test COMMAND mfp_fmirq_test)

# MFP ISRA/ISRB・VR Sビット（ソフトウェアEOI）の単体テスト
# （mfp_design.md §9）。mfp_fmirq_testと同じソース構成。
add_executable(mfp_isr_eoi_test
    tests/mfp_isr_eoi_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(mfp_isr_eoi_test PRIVATE src include)
target_link_libraries(mfp_isr_eoi_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME mfp_isr_eoi_test COMMAND mfp_isr_eoi_test)

# MFP(IRQ6)のIACKにベクタを渡せないときスプリアス割り込み(ベクタ24)へ倒れることの
# 単体テスト。mfp_isr_eoi_testと同じソース構成。
add_executable(mfp_spurious_iack_test
    tests/mfp_spurious_iack_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(mfp_spurious_iack_test PRIVATE src include)
target_link_libraries(mfp_spurious_iack_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME mfp_spurious_iack_test COMMAND mfp_spurious_iack_test)

# MFP USART TX（UDR書き込み = キーボードコマンド）の単体テスト（mfp_design.md §5.3）。
# mfp_spurious_iack_testと同じソース構成。
add_executable(mfp_usart_tx_test
    tests/mfp_usart_tx_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(mfp_usart_tx_test PRIVATE src include)
target_link_libraries(mfp_usart_tx_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME mfp_usart_tx_test COMMAND mfp_usart_tx_test)

# MFP Timer B(ch8)の単体テスト（mfp_design.md §9）。
# mfp_fmirq_testと同じソース構成。
add_executable(mfp_timerb_test
    tests/mfp_timerb_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(mfp_timerb_test PRIVATE src include)
target_link_libraries(mfp_timerb_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME mfp_timerb_test COMMAND mfp_timerb_test)

# MFPタイマのチャンク内同期・満了予定算出の単体テスト。mfp_timerb_testと同じソース構成。
add_executable(mfp_timer_sync_test
    tests/mfp_timer_sync_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(mfp_timer_sync_test PRIVATE src include)
target_link_libraries(mfp_timer_sync_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME mfp_timer_sync_test COMMAND mfp_timer_sync_test)

# GPIP bit7(H-SYNC)/bit4(V-DISP)のCRTCラスターモデル由来実装の単体テスト
# （mfp_design.md §9）。mfp_fmirq_testと同じソース構成。
add_executable(mfp_gpip_raster_test
    tests/mfp_gpip_raster_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(mfp_gpip_raster_test PRIVATE src include)
target_link_libraries(mfp_gpip_raster_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME mfp_gpip_raster_test COMMAND mfp_gpip_raster_test)

# CRTCラスターモデルの単体テスト。mfp_fmirq_testと同じソース構成。
add_executable(crtc_raster_test
    tests/crtc_raster_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(crtc_raster_test PRIVATE src include)
target_link_libraries(crtc_raster_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME crtc_raster_test COMMAND crtc_raster_test)

# CRTC動作ポート bit1（グラフィック画面高速クリア）の単体テスト。
# crtc_raster_testと同じソース構成（x68k_bus_create()経由のフルバス構成が必要）。
add_executable(crtc_rapid_clear_test
    tests/crtc_rapid_clear_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(crtc_rapid_clear_test PRIVATE src include)
target_link_libraries(crtc_rapid_clear_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME crtc_rapid_clear_test COMMAND crtc_rapid_clear_test)

# システムポート#5($E8E009)由来のRAM/ROMウェイトプロファイルとaccess_timing
# フックの単体テスト（cpu_design.md §2）。
# mfp_fmirq_testと同じソース構成。
add_executable(wait_profile_test
    tests/wait_profile_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(wait_profile_test PRIVATE src include)
target_link_libraries(wait_profile_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME wait_profile_test COMMAND wait_profile_test)

# RTC (RP5C15, $E8A000) の単体テスト。
# wait_profile_testと同じソース構成。
add_executable(rtc_rp5c15_test
    tests/rtc_rp5c15_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(rtc_rp5c15_test PRIVATE src include)
target_link_libraries(rtc_rp5c15_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME rtc_rp5c15_test COMMAND rtc_rp5c15_test)

add_executable(media_fp_test tests/media_fp_test.c src/peripheral/sha256.c src/peripheral/media_fp.c ${WIN_COMPAT_SRC})
target_include_directories(media_fp_test PRIVATE src include)
add_test(NAME media_fp_test COMMAND media_fp_test)

add_executable(guest_clock_test
    tests/guest_clock_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(guest_clock_test PRIVATE src include)
target_link_libraries(guest_clock_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME guest_clock_test COMMAND guest_clock_test)

add_executable(bus_snapshot_test
    tests/bus_snapshot_test.c
    src/bus/x68k_bus.c
    src/bus/snapshot.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sha256.c
    src/peripheral/media_fp.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(bus_snapshot_test PRIVATE src include)
target_link_libraries(bus_snapshot_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME bus_snapshot_test COMMAND bus_snapshot_test)

add_executable(page_map_test
    tests/page_map_test.c
    src/bus/x68k_bus.c
    src/bus/snapshot.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sha256.c
    src/peripheral/media_fp.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(page_map_test PRIVATE src include)
target_link_libraries(page_map_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME page_map_test COMMAND page_map_test)

add_executable(page_map_equiv_test
    tests/page_map_equiv_test.c
    src/bus/x68k_bus.c
    src/bus/snapshot.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sha256.c
    src/peripheral/media_fp.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(page_map_equiv_test PRIVATE src include)
target_link_libraries(page_map_equiv_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME page_map_equiv_test COMMAND page_map_equiv_test)

add_executable(checkpoint_test
    tests/checkpoint_test.c
    src/app/checkpoint.c
    src/bus/x68k_bus.c src/bus/snapshot.c src/bus/fpu_cir.c src/bus/sram.c
    src/peripheral/fdc.c src/peripheral/mfp.c src/peripheral/dmac.c src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c src/peripheral/opm.c src/audio/ym2151.c src/peripheral/crtc.c
    src/peripheral/rtc.c src/peripheral/sha256.c src/peripheral/media_fp.c src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c src/peripheral/sjis.c src/peripheral/hostfile.c
    src/peripheral/scsi.c src/peripheral/sasi.c src/peripheral/scc.c ${WIN_COMPAT_SRC}
)
target_include_directories(checkpoint_test PRIVATE src include)
target_link_libraries(checkpoint_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME checkpoint_test COMMAND checkpoint_test)

# システムポート#6 ($E8E00F) 電源OFF制御の単体テスト。
# rtc_rp5c15_testと同じソース構成。
add_executable(sysport_poweroff_test
    tests/sysport_poweroff_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(sysport_poweroff_test PRIVATE src include)
target_link_libraries(sysport_poweroff_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME sysport_poweroff_test COMMAND sysport_poweroff_test)

# CRTC動作ポート bit3（ラスタコピー）の水平フロントポーチ継続実行の単体テスト。
# crtc_rapid_clear_testと同じソース構成。
add_executable(crtc_raster_copy_test
    tests/crtc_raster_copy_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(crtc_raster_copy_test PRIVATE src include)
target_link_libraries(crtc_raster_copy_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME crtc_raster_copy_test COMMAND crtc_raster_copy_test)

# _B_PUTMESキャプチャの賞味期限（描画後スナップショットとの一致判定）の単体テスト。
# crtc_raster_copy_testと同じソース構成。
add_executable(putmes_overlay_test
    tests/putmes_overlay_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(putmes_overlay_test PRIVATE src include)
target_link_libraries(putmes_overlay_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME putmes_overlay_test COMMAND putmes_overlay_test)

# ANKフォント差し替え報告の単体テスト。putmes_overlay_testと
# 同じソース構成。
add_executable(ank_font_override_test
    tests/ank_font_override_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(ank_font_override_test PRIVATE src include)
target_link_libraries(ank_font_override_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME ank_font_override_test COMMAND ank_font_override_test)

# CGROM上位256KB($F80000-$FBFFFF)がIPLROMミラーに隠れないことの単体テスト。
add_executable(cgrom_upper_test
    tests/cgrom_upper_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(cgrom_upper_test PRIVATE src include)
target_compile_definitions(cgrom_upper_test PRIVATE X68K_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(cgrom_upper_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME cgrom_upper_test COMMAND cgrom_upper_test)

# cgrom_gen（BDFから互換CGROMを生成するツール）の単体テスト。合成した小さなBDFを使う。
add_executable(cgrom_gen_test tests/cgrom_gen_test.c)
if(MSVC)
    target_compile_definitions(cgrom_gen_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cgrom_gen_test PRIVATE /utf-8)
endif()
add_test(NAME cgrom_gen_test COMMAND cgrom_gen_test)

# 拡張スロット領域($EA0000-$EAFFFF)の読み出しデコードの単体テスト。
# HOSTDRV/HOSTFILEの裏口レジスタとRTCテストバイトがread8経路で実体に届くことを確認する
# （crtc_rapid_clear_testと同じくx68k_bus_create()経由のフルバス構成が必要）。
add_executable(expansion_mmio_test
    tests/expansion_mmio_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(expansion_mmio_test PRIVATE src include)
target_link_libraries(expansion_mmio_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME expansion_mmio_test COMMAND expansion_mmio_test)

# FPU-CIR(CZ-6BP1/6BP1A)の配置・XB1バスエラー境界。
add_executable(fpu_cir_window_test
    tests/fpu_cir_window_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(fpu_cir_window_test PRIVATE src include)
target_link_libraries(fpu_cir_window_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME fpu_cir_window_test COMMAND fpu_cir_window_test)

add_executable(local_memory_test tests/local_memory_test.c
    src/bus/x68k_bus.c src/bus/fpu_cir.c src/bus/sram.c src/peripheral/fdc.c src/peripheral/mfp.c
    src/peripheral/dmac.c src/peripheral/adpcm_codec.c src/peripheral/adpcm.c src/peripheral/opm.c src/audio/ym2151.c
    src/peripheral/crtc.c src/peripheral/rtc.c src/peripheral/sprite.c src/peripheral/hostdrv_remote.c src/peripheral/sjis.c
    src/peripheral/hostfile.c src/peripheral/scsi.c src/peripheral/sasi.c src/peripheral/scc.c ${WIN_COMPAT_SRC})
target_include_directories(local_memory_test PRIVATE src include)
target_link_libraries(local_memory_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME local_memory_test COMMAND local_memory_test)

# FPU-CIR adapter（実モデルでの応答・転送・save/restore）。
add_executable(fpu_cir_test
    tests/fpu_cir_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(fpu_cir_test PRIVATE src include)
target_link_libraries(fpu_cir_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME fpu_cir_test COMMAND fpu_cir_test)

# FLOAT3.X（純正ドライバ）の静的逆アセンブル由来 CIR 列の replay。
add_executable(fpu_cir_float3_test
    tests/fpu_cir_float3_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(fpu_cir_float3_test PRIVATE src include)
target_link_libraries(fpu_cir_float3_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME fpu_cir_float3_test COMMAND fpu_cir_float3_test)

# FLOAT3.X を実際に常駐させる E2E（headless ix68k + MCP ワイヤ）。disk/FLOAT3.XDF
# （tools-host/dev/make_float3_xdf.sh で生成。disk/ は git 管理外）・ROM・ヘッドレスのビルドが無い環境では
# 終了コード 77 で SKIP になる。SDL 版では実行しない（画面が開くため）。
if(PLATFORM_NULL)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME fpu_cir_float3_e2e
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_fpu_cir_float3_e2e.py)
        set_tests_properties(fpu_cir_float3_e2e PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 180
            ENVIRONMENT "IX68K_BIN=$<TARGET_FILE:ix68k>")
        add_test(NAME fpu_fpcp_float4_e2e
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_fpu_fpcp_float4_e2e.py)
        set_tests_properties(fpu_fpcp_float4_e2e PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 180
            ENVIRONMENT "IX68K_BIN=$<TARGET_FILE:ix68k>")
    endif()
endif()

# MCPデバッガコマンドの単体テスト（docs/ix68k-internals/design/mcp_design.md §5）。
# 非侵襲peek配線・逆アセンブルを検証するためmc68000コアもリンクする。
add_executable(debugger_test
    tests/debugger_test.c
    src/mcp/debugger.c
    src/mcp/perf.c
    src/mcp/dbg_cond.c
    src/platform/platform_null.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(debugger_test PRIVATE src include)
target_link_libraries(debugger_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME debugger_test COMMAND debugger_test)

# MCPワイヤコマンド（mcp_commands_handle）の単体テスト。
# server.cはリンクせず、mcp_reply/mcp_reply_bin とアプリ層グローバルを
# テスト側で定義して応答文字列を捕まえる。SDLは不要（platform_nullを直接使う）。
add_executable(mcp_commands_test
    tests/mcp_commands_test.c
    src/app/checkpoint.c
    src/mcp/commands.c
    src/mcp/text_event.c
    src/mcp/text_probe.c
    src/mcp/debugger.c
    src/mcp/perf.c
    src/mcp/dbg_cond.c
    src/platform/platform_null.c
    src/bus/x68k_bus.c
    src/bus/snapshot.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sha256.c
    src/peripheral/media_fp.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(mcp_commands_test PRIVATE src include)
target_link_libraries(mcp_commands_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME mcp_commands_test COMMAND mcp_commands_test)

# MCPサーバの実ソケット結合テスト。server.cをリンクし、
# 同一プロセスの非ブロッキングclientでpipeline・切断復旧を検証する（POSIXのみ）。
if(NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(mcp_server_pipeline_test
        tests/mcp_server_pipeline_test.c
        src/mcp/server.c
        src/mcp/sock_path.c
        src/mcp/port.c
        src/mcp/commands.c
        src/app/checkpoint.c
        src/mcp/text_event.c
        src/mcp/text_probe.c
        src/mcp/debugger.c
        src/mcp/perf.c
        src/mcp/dbg_cond.c
        src/platform/platform_null.c
        src/bus/x68k_bus.c
        src/bus/snapshot.c
        src/bus/fpu_cir.c
        src/bus/sram.c
        src/peripheral/fdc.c
        src/peripheral/mfp.c
        src/peripheral/dmac.c
        src/peripheral/adpcm_codec.c
        src/peripheral/adpcm.c
        src/peripheral/opm.c
        src/audio/ym2151.c
        src/peripheral/crtc.c
        src/peripheral/rtc.c
        src/peripheral/sha256.c
        src/peripheral/media_fp.c
        src/peripheral/sprite.c
        src/peripheral/hostdrv_remote.c
        src/peripheral/sjis.c
        src/peripheral/hostfile.c
        src/peripheral/scsi.c
        src/peripheral/sasi.c
        src/peripheral/scc.c
    )
    target_include_directories(mcp_server_pipeline_test PRIVATE src include)
    target_link_libraries(mcp_server_pipeline_test PRIVATE mc68000 ${ICONV_LIBRARY} Threads::Threads)
    add_test(NAME mcp_server_pipeline_test COMMAND mcp_server_pipeline_test)
endif()

add_executable(text_event_test tests/text_event_test.c src/mcp/text_event.c)
target_include_directories(text_event_test PRIVATE src include extern/cmc68000/src)
target_link_libraries(text_event_test PRIVATE mc68000)
add_test(NAME text_event_test COMMAND text_event_test)

add_executable(perf_counter_test tests/perf_counter_test.c src/mcp/perf.c)
target_include_directories(perf_counter_test PRIVATE src include extern/cmc68000/src)
target_link_libraries(perf_counter_test PRIVATE mc68000)
add_test(NAME perf_counter_test COMMAND perf_counter_test)

add_executable(text_probe_test tests/text_probe_test.c src/mcp/text_event.c src/mcp/text_probe.c)
target_include_directories(text_probe_test PRIVATE src include extern/cmc68000/src)
target_link_libraries(text_probe_test PRIVATE mc68000)
add_test(NAME text_probe_test COMMAND text_probe_test)

# ラスタ同期レンダリングの回帰テスト。crtc_raster_testと同じ
# ソース構成に、スプライトレンダラー(x68k_render_sp/_line)を追加。
add_executable(raster_sync_test
    tests/raster_sync_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    src/render/sprite_renderer.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(raster_sync_test PRIVATE src include)
target_link_libraries(raster_sync_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME raster_sync_test COMMAND raster_sync_test)

# DMAC ch3(ADPCM)の実時間ペーシングの単体テスト。バス層+ADPCMを
# 通すためmfp_fmirq_testと同じソース構成でリンクする。
add_executable(adpcm_dma_test
    tests/adpcm_dma_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(adpcm_dma_test PRIVATE src include)
target_link_libraries(adpcm_dma_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME adpcm_dma_test COMMAND adpcm_dma_test)

# DMAC(HD63450)がRESET命令で正しく初期化されることの単体テスト。
# adpcm_dma_testと同じソース構成でリンクする。
add_executable(dmac_reset_test
    tests/dmac_reset_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(dmac_reset_test PRIVATE src include)
target_link_libraries(dmac_reset_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME dmac_reset_test COMMAND dmac_reset_test)

# DMAC ch2メモリ間転送が転送時間を持つことの単体テスト。dmac_reset_testと同じソース構成。
add_executable(dmac_ch2_timing_test
    tests/dmac_ch2_timing_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(dmac_ch2_timing_test PRIVATE src include)
target_link_libraries(dmac_ch2_timing_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME dmac_ch2_timing_test COMMAND dmac_ch2_timing_test)

# DMAC IACKが要求チャネルのNIVをライブ参照することの単体テスト。
# dmac_reset_testと同じバス層のソース構成でリンクする。
add_executable(dmac_iack_test
    tests/dmac_iack_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(dmac_iack_test PRIVATE src include)
target_link_libraries(dmac_iack_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME dmac_iack_test COMMAND dmac_iack_test)

# FDC(µPD72065)がRESET命令/電源投入時に正しく初期化されることの単体テスト。
# dmac_reset_testと同じソース構成でリンクする。
add_executable(fdc_reset_test
    tests/fdc_reset_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(fdc_reset_test PRIVATE src include)
target_link_libraries(fdc_reset_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME fdc_reset_test COMMAND fdc_reset_test)

# $E94005 bit6(EJECT MASK)の単体テスト。
add_executable(fdc_eject_test
    tests/fdc_eject_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(fdc_eject_test PRIVATE src include)
target_link_libraries(fdc_eject_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME fdc_eject_test COMMAND fdc_eject_test)

# ディスク選択ウィンドウ向けFDC状態アクセサの単体テスト。
add_executable(fdc_status_test
    tests/fdc_status_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(fdc_status_test PRIVATE src include)
target_link_libraries(fdc_status_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME fdc_status_test COMMAND fdc_status_test)

# ジョイスティック(PPIポートA/B)の単体テスト。
add_executable(joystick_ppi_test
    tests/joystick_ppi_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(joystick_ppi_test PRIVATE src include)
target_link_libraries(joystick_ppi_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME joystick_ppi_test COMMAND joystick_ppi_test)

# SCSI状態アクセサとコマンド応答の単体テスト。
add_executable(scsi_status_test
    tests/scsi_status_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(scsi_status_test PRIVATE src include)
target_link_libraries(scsi_status_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME scsi_status_test COMMAND scsi_status_test)

# SCSI CD-ROM(CloneCD)メディア層の単体テスト(P1)。
add_executable(scsi_cdrom_test
    tests/scsi_cdrom_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(scsi_cdrom_test PRIVATE src include)
target_link_libraries(scsi_cdrom_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME scsi_cdrom_test COMMAND scsi_cdrom_test)

# SCSI MO(リムーバブルメディア)ライトプロテクトの単体テスト。
add_executable(scsi_wp_test
    tests/scsi_wp_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(scsi_wp_test PRIVATE src include)
target_link_libraries(scsi_wp_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME scsi_wp_test COMMAND scsi_wp_test)

# SPCプログラム転送(DREG連続書き込み)によるCOMMANDフェーズの単体テスト。
add_executable(scsi_dreg_cdb_test
    tests/scsi_dreg_cdb_test.c
    src/bus/x68k_bus.c
    src/bus/fpu_cir.c
    src/bus/sram.c
    src/peripheral/fdc.c
    src/peripheral/mfp.c
    src/peripheral/dmac.c
    src/peripheral/adpcm_codec.c
    src/peripheral/adpcm.c
    src/peripheral/opm.c
    src/audio/ym2151.c
    src/peripheral/crtc.c
    src/peripheral/rtc.c
    src/peripheral/sprite.c
    src/peripheral/hostdrv_remote.c
    src/peripheral/sjis.c
    src/peripheral/hostfile.c
    src/peripheral/scsi.c
    src/peripheral/sasi.c
    src/peripheral/scc.c
    ${WIN_COMPAT_SRC}
)
target_include_directories(scsi_dreg_cdb_test PRIVATE src include)
target_link_libraries(scsi_dreg_cdb_test PRIVATE mc68000 ${ICONV_LIBRARY})
add_test(NAME scsi_dreg_cdb_test COMMAND scsi_dreg_cdb_test)

# x68k_render_sp() の単体テスト（docs/ix68k-internals/design/sprite_design.md §9）。
add_executable(sprite_render_test
    tests/sprite_render_test.c
    src/render/gvram_renderer.c
    src/render/tvram_renderer.c
    src/render/sprite_renderer.c
    src/peripheral/crtc.c
    src/peripheral/sprite.c
    src/peripheral/mfp.c
)
target_include_directories(sprite_render_test PRIVATE src include)
target_link_libraries(sprite_render_test PRIVATE mc68000)
add_test(NAME sprite_render_test COMMAND sprite_render_test)

# x68k_bus_get_display_size() / x68k_bus_is_square_dot_mode() の単体テスト。
add_executable(crtc_display_size_test
    tests/crtc_display_size_test.c
    src/peripheral/crtc.c
    src/peripheral/sprite.c
    src/peripheral/mfp.c
)
target_include_directories(crtc_display_size_test PRIVATE src include)
target_link_libraries(crtc_display_size_test PRIVATE mc68000)
add_test(NAME crtc_display_size_test COMMAND crtc_display_size_test)

# グラフィック画面65536色モードのグラフィックパレット変換。
add_executable(gfx_palette_64k_test
    tests/gfx_palette_64k_test.c
    src/render/gvram_renderer.c
)
target_include_directories(gfx_palette_64k_test PRIVATE src include)
target_link_libraries(gfx_palette_64k_test PRIVATE mc68000)
add_test(NAME gfx_palette_64k_test COMMAND gfx_palette_64k_test)

# システムポート#1 ($E8E001 CONTRAST) LUTの単体テスト。
add_executable(contrast_test
    tests/contrast_test.c
    src/peripheral/crtc.c
    src/peripheral/sprite.c
    src/peripheral/mfp.c
)
target_include_directories(contrast_test PRIVATE src include)
target_link_libraries(contrast_test PRIVATE mc68000)
add_test(NAME contrast_test COMMAND contrast_test)

# R2半透明/特殊プライオリティ: 生コードsampler、モードデコード、GRB555合成。
add_executable(video_r2_effects_test
    tests/video_r2_effects_test.c
    src/render/compose.c
    src/render/gvram_renderer.c
    src/render/tvram_renderer.c
    src/render/sprite_renderer.c
    src/render/video_effects.c
    src/peripheral/crtc.c
    src/peripheral/sprite.c
    src/peripheral/mfp.c
)
target_include_directories(video_r2_effects_test PRIVATE src include)
target_link_libraries(video_r2_effects_test PRIVATE mc68000)
add_test(NAME video_r2_effects_test COMMAND video_r2_effects_test)

# 実画面1024ドット時のGVRAMアドレス計算（リニア2M面＋R1のバンク選択）。
add_executable(gvram_1024_test
    tests/gvram_1024_test.c
    src/render/gvram_renderer.c
)
target_include_directories(gvram_1024_test PRIVATE src include)
target_link_libraries(gvram_1024_test PRIVATE mc68000)
add_test(NAME gvram_1024_test COMMAND gvram_1024_test)

# 分割した内部責務モジュールを、対応する本体を使う全ターゲットへ追加する。
# テストごとの重複したソース列挙を変更せず、依存関係をここで一元化する。
get_property(X68K_LOCAL_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(X68K_TARGET IN LISTS X68K_LOCAL_TARGETS)
    get_target_property(X68K_TARGET_SOURCES ${X68K_TARGET} SOURCES)
    if(X68K_TARGET_SOURCES)
        list(FIND X68K_TARGET_SOURCES "src/peripheral/hostdrv_remote.c" X68K_HAS_HOSTDRV_REMOTE)
        if(NOT X68K_HAS_HOSTDRV_REMOTE EQUAL -1)
            target_sources(${X68K_TARGET} PRIVATE src/peripheral/hostdrv_remote_fd.c)
        endif()
        list(FIND X68K_TARGET_SOURCES "src/peripheral/scsi.c" X68K_HAS_SCSI)
        if(NOT X68K_HAS_SCSI EQUAL -1)
            target_sources(${X68K_TARGET} PRIVATE src/peripheral/scsi_media.c src/peripheral/sha256.c
                           src/peripheral/media_fp.c)
        endif()
        list(FIND X68K_TARGET_SOURCES "src/peripheral/rtc.c" X68K_HAS_RTC)
        if(NOT X68K_HAS_RTC EQUAL -1)
            target_sources(${X68K_TARGET} PRIVATE src/bus/guest_clock.c)
        endif()
        list(FIND X68K_TARGET_SOURCES "src/audio/ym2151.c" X68K_HAS_YM2151)
        if(NOT X68K_HAS_YM2151 EQUAL -1)
            target_sources(${X68K_TARGET} PRIVATE src/audio/ym2151_timer.c)
        endif()
    endif()
endforeach()
