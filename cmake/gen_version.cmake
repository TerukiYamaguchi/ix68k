# x68k_version.h を生成する（ビルドのたびに実行し、内容が変わったときだけ書き換える）。
# 入力: SOURCE_DIR / OUTPUT / RELEASE_VERSION（空でなければそのまま使う）
# リリース版は stage.sh / stage.ps1 が渡す日付（例: 20261125）、日々のビルドは
# git describe（タグ・コミット数・ハッシュ、未コミット変更があれば -dirty）。
set(version "${RELEASE_VERSION}")
if(version STREQUAL "")
    execute_process(
        COMMAND git describe --tags --always --dirty
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE git_result)
    if(NOT git_result EQUAL 0 OR version STREQUAL "")
        set(version "unknown")
    endif()
endif()

set(content "#define X68K_VERSION_STRING \"${version}\"\n")
set(current "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" current)
endif()
if(NOT content STREQUAL current)
    file(WRITE "${OUTPUT}" "${content}")
endif()
