# Release ZIP (Windows x64) をローカルで組み立てて検証する（Issue #195）。
# 同梱物の正本は packaging/release/release_package_contents.md。
#
# 使い方: powershell -File packaging\release\stage.ps1 [-Version YYYYMMDD] [-VcpkgRoot C:\vcpkg]
#   バージョン省略時は実行日（YYYYMMDD）。出力は build\release\ix68k-windows-x64-<バージョン>.zip
#   （ZIP 内のトップフォルダは日付なしの ix68k-windows-x64）。
# 前提: Visual Studio Build Tools 2022、CMake 3.21+、vcpkg の sdl3(x64-windows)、Python 3。
#   clang-cl（LLVM 公式の winget `LLVM.LLVM` など）があれば、既定でそれを使う（Issue #268）。
#   -Compiler clang-cl|msvc で明示できる。clang-cl では packaging\release\pgo\ix68k-windows-x64.profdata を
#   適用する（採り直しは未対応。MSVC は PGO なし）。
param(
    [string]$Version = (Get-Date -Format "yyyyMMdd"),
    [ValidateSet("auto", "clang-cl", "msvc")][string]$Compiler = "auto",
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\vcpkg" })
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$OutDir = Join-Path $Root "build\release"
$BuildDir = Join-Path $OutDir "cmake-build"
$InstallDir = Join-Path $OutDir "install"
$StageRoot = Join-Path $OutDir "stage"
$Name = "ix68k-windows-x64"
$PkgDir = Join-Path $StageRoot $Name
$ZipPath = Join-Path $OutDir "$Name-$Version.zip"
$HostdrvSys = Join-Path $Root "tools-x68k\hostdrv\HOSTDRV.SYS"
$HostfileX = Join-Path $Root "tools-x68k\hostfile\hostfile.x"
$JoygetTestX = Join-Path $Root "tools-x68k\joyget_test\joyget_test.x"
$GedX = Join-Path $Root "tools-x68k\ged\GED.X"
$AiqX = Join-Path $Root "tools-x68k\aiq\aiq.x"
$FntadrX = Join-Path $Root "tools-x68k\fntadr\fntadr.x"
$ToolsHostDir = "tools-host"
$ToolsX68kDir = "tools-x68k"
# ZIPに含めてはならないROM・メディアの拡張子
# ZIPに入れる例外: フォーマット済みの空2HDイメージ（ROM・Human68kを含まない雛形）
$BlankXdf = Join-Path $Root "packaging\release\BLANK.XDF"
$ForbiddenExt = '\.(dat|rom|xdf|hds|hdf|dim|2hd|d88|iso)$'

function Log($msg) { Write-Host "[stage] $msg" }
function Die($msg) { throw "[stage] ERROR: $msg" }
function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { Die "$Exe が失敗した（exit $LASTEXITCODE）: $($Arguments -join ' ')" }
}

function Import-MsvcEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { Die "Visual Studio Build Tools が見つからない" }
    $vsPath = & $vswhere -latest -products * -property installationPath
    $bat = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $bat)) { Die "vcvars64.bat が見つからない: $bat" }
    cmd /c "`"$bat`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

# clang-cl を探す。PATH → LLVM 既定の導入先 → VS の C++ Clang ツールの順。
function Find-ClangCl {
    $cmd = Get-Command clang-cl.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $cands = @((Join-Path $env:ProgramFiles "LLVM\bin\clang-cl.exe"))
    if ($env:VCINSTALLDIR) { $cands += (Join-Path $env:VCINSTALLDIR "Tools\Llvm\x64\bin\clang-cl.exe") }
    foreach ($c in $cands) { if (Test-Path $c) { return $c } }
    return $null
}

function Get-Dependents([string]$File) {
    & dumpbin /dependents $File | Select-String -Pattern '^\s+(\S+\.dll)\s*$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
}

# SDL3.dll（vcpkg版）も動的CRTを使うため、/MT では解消しない。VCランタイムDLLは
# アプリローカル配置（再配布可）として、VSのredistから ZIP トップへ同梱する。
function Copy-VcRuntime {
    $redist = Join-Path $env:VCToolsRedistDir "x64\Microsoft.VC143.CRT"
    if (-not (Test-Path $redist)) { Die "VC redist が見つからない: $redist" }
    $vcDll = '^(vcruntime|msvcp|concrt|vcomp)'
    foreach ($bin in "ix68k.exe", "SDL3.dll") {
        foreach ($dll in (Get-Dependents (Join-Path $PkgDir $bin)) | Where-Object { $_ -match $vcDll }) {
            Copy-Item (Join-Path $redist $dll) $PkgDir -Force
            Log "VCランタイムを同梱: $dll"
        }
    }
}

# リリースビルドの前に PGO プロファイルを採り直す（stage.sh の train_pgo_profile と同じ方針）。
# clang-cl のときだけ。採取には ROM・ディスクを使う local\profiles が要る。無い環境では
# 採取せず同梱プロファイルを使う。$env:X68K_PGO_TRAIN=0 でも省く。失敗したら中止する。
function Train-PgoProfile($toolchain) {
    if (-not $script:ClangCl) { return }
    if ($env:X68K_PGO_TRAIN -eq "0") { Log "PGO プロファイルの採取を省略（X68K_PGO_TRAIN=0）"; return }
    if (-not (Test-Path (Join-Path $Root "local\profiles\disk.cfg"))) {
        Log "PGO 採取の前提（local\profiles\disk.cfg）が無いため、同梱プロファイルを使う"
        return
    }
    $genDir = Join-Path $OutDir "pgo-gen"
    $fresh = Join-Path $OutDir "fresh.profdata"
    $pgoProfile = Join-Path $Root "packaging\release\pgo\$Name.profdata"
    Log "PGO プロファイルを採取する（計装ビルド → GCC.X BENCH）"
    if (Test-Path $genDir) { Remove-Item $genDir -Recurse -Force }
    if (Test-Path $fresh) { Remove-Item $fresh -Force }
    Invoke-Native cmake @("-S", $Root, "-B", $genDir, "-G", "NMake Makefiles",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DCMAKE_C_COMPILER=$($script:ClangCl)", "-DX68K_PGO_GENERATE=ON")
    Invoke-Native cmake @("--build", $genDir, "--target", "ix68k")
    Invoke-Native python @((Join-Path $Root "tools-host\pgo\train.py"), (Join-Path $genDir "ix68k.exe"), $fresh)
    Move-Item $fresh $pgoProfile -Force
    Log "PGO プロファイルを更新: $pgoProfile（packaging\release\pgo\README.md の採取情報も更新してコミットすること）"
}

function Build-And-Install {
    $toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $toolchain)) { Die "vcpkg が見つからない: $VcpkgRoot" }
    Train-PgoProfile $toolchain
    $cmakeArgs = @("-S", $Root, "-B", $BuildDir, "-G", "NMake Makefiles",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DX68K_APP_VERSION=$Version", "-DX68K_RELEASE_VERSION=$Version")
    if ($script:ClangCl) {
        $cmakeArgs += "-DCMAKE_C_COMPILER=$($script:ClangCl)"
        $pgoProfile = Join-Path $Root "packaging\release\pgo\$Name.profdata"
        if (Test-Path $pgoProfile) {
            Log "PGO プロファイルを適用: $pgoProfile"
            $cmakeArgs += "-DX68K_PGO_PROFILE=$pgoProfile"
        } else {
            Log "PGO プロファイル無し（通常ビルド）"
        }
    } else {
        Log "MSVC でビルドする（PGO なし）"
    }
    Invoke-Native cmake $cmakeArgs
    # 配布する ix68k だけを先にビルドして不一致を数える（テスト用ターゲットは別の定義で
    # コンパイルされ、全体ビルドの警告数は指標にならない。stage.sh と同じ方式）。
    # clang の警告は stderr に出るため、$ErrorActionPreference=Stop の影響を避けて cmd 経由でログへ集める。
    $buildLog = Join-Path $OutDir "build_ix68k.log"
    & cmd /c "cmake --build `"$BuildDir`" --parallel --target ix68k > `"$buildLog`" 2>&1"
    $buildExit = $LASTEXITCODE
    Get-Content $buildLog | Write-Host
    if ($buildExit -ne 0) { Die "ix68k のビルドが失敗した（exit $buildExit）" }
    if ($script:ClangCl -and (Test-Path (Join-Path $Root "packaging\release\pgo\$Name.profdata"))) {
        $n = @(Select-String -Path $buildLog -Pattern "profile-instr-out-of-date").Count
        Log "ix68k の profile-instr-out-of-date 警告: $n 件"
    }
    Invoke-Native cmake @("--build", $BuildDir, "--parallel")
    Invoke-Native ctest @("--test-dir", $BuildDir, "--output-on-failure")
    Invoke-Native cmake @("--install", $BuildDir, "--prefix", $InstallDir)
}

# ホストとHuman68k間のファイル転送CLI。Python（標準ライブラリのみ）なのでスクリプトのまま入れる。
# 同じ ZIP の tools-host\mcp\ix68k_wire.py を読むため、mcp\ は Stage-Mcp が入れる。
function Stage-HostfileCli {
    $dest = Join-Path $PkgDir "$ToolsHostDir\ix68k-hostfile"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item (Join-Path $Root "tools-host\ix68k-hostfile\ix68k-hostfile") $dest
    Copy-Item (Join-Path $Root "tools-host\ix68k-hostfile\README.md") (Join-Path $dest "README.md")
}

# hu68disk: CMake でビルド済みの hu68disk.exe を tools-host\hu68disk\ へ置く（macOS / Linux は stage.sh が Makefile でビルド）
function Stage-Hu68disk {
    $exe = Join-Path $BuildDir "hu68disk.exe"
    if (-not (Test-Path $exe)) { Die "hu68disk.exe が無い: $exe" }
    $dest = Join-Path $PkgDir "$ToolsHostDir\hu68disk"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item $exe $dest
    Copy-Item (Join-Path $Root "tools-host\hu68disk\README.md") $dest
}

# sram_switch: CMake でビルド済みの sram_switch.exe を tools-host\sram_switch\ へ置く（Issue #115）
function Stage-SramSwitch {
    $exe = Join-Path $BuildDir "sram_switch.exe"
    if (-not (Test-Path $exe)) { Die "sram_switch.exe が無い: $exe" }
    $dest = Join-Path $PkgDir "$ToolsHostDir\sram_switch"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item $exe $dest
    Copy-Item (Join-Path $Root "tools-host\sram_switch\README.md") $dest
}

# tools-x68k\<dir>\ へゲスト用バイナリと README.TXT を置く
function Stage-X68kTool($Dir, $Src, $FileName) {
    if (-not (Test-Path $Src)) { Die "ゲスト用バイナリが無い: $Src" }
    $dest = Join-Path $PkgDir "$ToolsX68kDir\$Dir"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item $Src (Join-Path $dest $FileName)
    Copy-Item (Join-Path (Split-Path $Src) "README.TXT") (Join-Path $dest "README.TXT")
}

# cgrom_gen: CMake でビルド済みの cgrom_gen.exe を tools-host\cgrom_gen\ へ置く（BDFから互換CGROMを作る）
function Stage-CgromGen {
    $exe = Join-Path $BuildDir "cgrom_gen.exe"
    if (-not (Test-Path $exe)) { Die "cgrom_gen.exe が無い: $exe" }
    $dest = Join-Path $PkgDir "$ToolsHostDir\cgrom_gen"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item $exe $dest
    Copy-Item (Join-Path $Root "tools-host\cgrom_gen\README.md") $dest
}

# MCPサーバ一式（ビルドせずClaude Code等からix68kを操作する人向け）。
# リポジトリ直下の .mcp.json（tools-host\mcp を指す）をそのまま入れる（展開先で claude を起動すれば自動で読まれる）。
function Stage-Mcp {
    $dest = Join-Path $PkgDir "$ToolsHostDir\mcp"
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach ($f in "ix68k_mcp_server.py", "ix68k_wire.py", "pyproject.toml", "uv.lock", "MCP_TOOLS.md", "README.md") {
        Copy-Item (Join-Path $Root "tools-host\mcp\$f") $dest
    }
    Copy-Item (Join-Path $Root "tools-host\mcp\text_profiles") $dest -Recurse
    Copy-Item (Join-Path $Root ".mcp.json") (Join-Path $PkgDir ".mcp.json")
}

function Assemble-Package {
    foreach ($d in "rom", "disk", "docs") {
        New-Item -ItemType Directory -Force (Join-Path $PkgDir $d) | Out-Null
    }
    Copy-Item (Join-Path $InstallDir "*") $PkgDir -Recurse -Force
    Copy-VcRuntime
    Stage-HostfileCli
    Stage-Hu68disk
    Stage-SramSwitch
    Stage-X68kTool "hostdrv" $HostdrvSys "HOSTDRV.SYS"
    Stage-X68kTool "hostfile" $HostfileX "HOSTFILE.X"
    Stage-X68kTool "joyget_test" $JoygetTestX "JOYGET_TEST.X"
    Stage-X68kTool "ged" $GedX "GED.X"
    Stage-X68kTool "aiq" $AiqX "AIQ.X"
    Stage-X68kTool "fntadr" $FntadrX "FNTADR.X"
    if (-not (Test-Path $BlankXdf)) { Die "BLANK.XDF が無い: $BlankXdf" }
    Copy-Item $BlankXdf (Join-Path $PkgDir "disk\BLANK.XDF")
    Stage-CgromGen
    Stage-Mcp
    Copy-Item (Join-Path $Root "LICENSES") $PkgDir -Recurse
    Copy-Item (Join-Path $Root "profiles\ix68k.cfg.example"), (Join-Path $Root "profiles\keyboard.cfg.example") $PkgDir
    Copy-Item (Join-Path $Root "packaging\release\dist_readme.md") (Join-Path $PkgDir "README.md")
    Copy-Item (Join-Path $Root "docs\x68k-usage") (Join-Path $PkgDir "docs") -Recurse
    $license = Join-Path $Root "LICENSE"
    if (Test-Path $license) { Copy-Item $license $PkgDir } else { Log "WARN: LICENSE が未作成のため同梱していない" }
}

# Compress-Archive (Windows PowerShell 5.1) はパス区切りに '\' を使い、空ディレクトリも
# 落とすため、ZipArchive で '/' 区切りのエントリと空ディレクトリを明示的に書く。
function Make-Zip {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    $zip = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $base = (Resolve-Path $PkgDir).Path.TrimEnd('\')
        foreach ($item in Get-ChildItem $PkgDir -Recurse -Force) {
            $rel = $item.FullName.Substring($base.Length + 1).Replace('\', '/')
            if ($item.PSIsContainer) {
                if (-not (Get-ChildItem $item.FullName -Force)) {
                    [void]$zip.CreateEntry("$Name/$rel/")
                }
            } else {
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, $item.FullName, "$Name/$rel", [System.IO.Compression.CompressionLevel]::Optimal)
            }
        }
    } finally { $zip.Dispose() }
}

function Verify-Zip {
    $checkDir = Join-Path $OutDir "verify"
    if (Test-Path $checkDir) { Remove-Item $checkDir -Recurse -Force }
    New-Item -ItemType Directory $checkDir | Out-Null
    [System.IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $checkDir)
    $pkg = Join-Path $checkDir $Name

    $required = @("ix68k.exe", "SDL3.dll", "gamecontrollerdb.txt", "tools-host\ix68k-hostfile\ix68k-hostfile", "tools-host\ix68k-hostfile\README.md",
        "tools-host\hu68disk\hu68disk.exe", "tools-host\hu68disk\README.md",
        "tools-host\sram_switch\sram_switch.exe", "tools-host\sram_switch\README.md",
        "tools-x68k\hostdrv\HOSTDRV.SYS", "tools-x68k\hostfile\HOSTFILE.X", "tools-x68k\joyget_test\JOYGET_TEST.X",
        "tools-x68k\ged\GED.X", "tools-x68k\aiq\AIQ.X", "tools-x68k\fntadr\FNTADR.X",
        "tools-x68k\hostdrv\README.TXT", "tools-x68k\hostfile\README.TXT", "tools-x68k\joyget_test\README.TXT",
        "tools-x68k\ged\README.TXT", "tools-x68k\aiq\README.TXT", "tools-x68k\fntadr\README.TXT", "LICENSES\ymfm-BSD-3-Clause.txt", "LICENSES\SoftFloat-BSD-3-Clause.txt", "LICENSES\XEiJ-License.txt", "LICENSES\Sharp-Products-Users-Forum-License.txt", "ix68k.cfg.example", "keyboard.cfg.example", "README.md", ".mcp.json",
        "tools-host\mcp\ix68k_mcp_server.py", "tools-host\mcp\ix68k_wire.py", "tools-host\mcp\pyproject.toml", "tools-host\mcp\uv.lock",
        "tools-host\mcp\MCP_TOOLS.md", "tools-host\mcp\text_profiles", "tools-host\mcp\README.md",
        "tools-host\cgrom_gen\cgrom_gen.exe", "tools-host\cgrom_gen\README.md",
        "rom", "disk\BLANK.XDF", "docs\x68k-usage")
    foreach ($p in $required) {
        if (-not (Test-Path (Join-Path $pkg $p))) { Die "ZIPに必要なパスが無い: $p" }
    }
    if (Get-ChildItem (Join-Path $pkg "rom") -Force) { Die "rom\ が空でない" }
    $diskFiles = @(Get-ChildItem (Join-Path $pkg "disk") -Force)
    if ($diskFiles.Count -ne 1 -or $diskFiles[0].Name -ne "BLANK.XDF") { Die "disk\ が BLANK.XDF だけでない" }
    if ((Get-FileHash $BlankXdf).Hash -ne (Get-FileHash $diskFiles[0].FullName).Hash) { Die "disk\BLANK.XDF が雛形と一致しない" }
    $entries = [System.IO.Compression.ZipFile]::OpenRead($ZipPath).Entries.FullName
    $bad = $entries | Where-Object { $_ -match $ForbiddenExt -and $_ -notmatch '/disk/BLANK\.XDF$' }
    if ($bad) { $bad | ForEach-Object { Write-Host $_ }; Die "ROM・メディアらしきファイルがZIPに含まれている" }
    if ($entries | Where-Object { $_ -like '*\*' }) { Die "ZIPエントリに '\' 区切りが混入している" }

    # 依存DLLが ZIP 内か OS 標準のものだけであることを確認する
    $deps = Get-Dependents (Join-Path $pkg "ix68k.exe")
    Log ("ix68k.exe の依存DLL: " + ($deps -join ", "))
    foreach ($dll in $deps) {
        if ($dll -match '^(api-ms-win-|ext-ms-win-)') { continue }
        if (Test-Path (Join-Path $pkg $dll)) { continue }
        if (Test-Path (Join-Path $env:SystemRoot "System32\$dll")) {
            if ($dll -match '^(vcruntime|msvcp|concrt|vcomp|ucrtbase)') {
                Die "VCランタイム($dll)がZIPに無い（クリーン環境で起動できない可能性）"
            }
            continue
        }
        Die "未解決の依存DLL: $dll"
    }
    Invoke-Native python @((Join-Path $pkg "$ToolsHostDir\ix68k-hostfile\ix68k-hostfile"), "--help") | Out-Null
    Invoke-Native (Join-Path $pkg "$ToolsHostDir\cgrom_gen\cgrom_gen.exe") @("--version") | Out-Null
    # hu68disk: 空イメージ作成→日本語名ファイル追加→取り出しの往復で動作を確認する
    $hu = Join-Path $checkDir "hu68disk_smoke"
    New-Item -ItemType Directory $hu | Out-Null
    $huExe = Join-Path $pkg "$ToolsHostDir\hu68disk\hu68disk.exe"
    $huIn = Join-Path $hu "テスト.x"
    $huOut = Join-Path $hu "out.x"
    [System.IO.File]::WriteAllText($huIn, "hu68disk smoke`n")
    Invoke-Native $huExe @("mkfs", (Join-Path $hu "t.xdf"), "--force") | Out-Null
    Invoke-Native $huExe @("copy", (Join-Path $hu "t.xdf"), $huIn, "/") | Out-Null
    Invoke-Native $huExe @("extract", (Join-Path $hu "t.xdf"), "/テスト.x", $huOut) | Out-Null
    if ((Get-FileHash $huIn).Hash -ne (Get-FileHash $huOut).Hash) { Die "hu68disk の往復でデータが一致しない" }
    # sram_switch: 空の sram.dat に設定して読み戻す
    $sr = Join-Path $checkDir "sram_smoke.dat"
    [System.IO.File]::WriteAllBytes($sr, (New-Object byte[] 16384))
    $srExe = Join-Path $pkg "$ToolsHostDir\sram_switch\sram_switch.exe"
    Invoke-Native $srExe @("-f", $sr, "B=2HD0") | Out-Null
    if (-not ((Invoke-Native $srExe @("-f", $sr)) -match "2HD0")) { Die "sram_switch の設定が読み戻せない" }
    Log "検証OK: $ZipPath"
}

Import-MsvcEnvironment
$script:ClangCl = $null
if ($Compiler -ne "msvc") {
    $script:ClangCl = Find-ClangCl
    if (-not $script:ClangCl -and $Compiler -eq "clang-cl") { Die "clang-cl が見つからない（winget install LLVM.LLVM）" }
    if ($script:ClangCl) {
        Log "clang-cl を使う: $($script:ClangCl)"
        $env:PATH = (Split-Path $script:ClangCl) + ";" + $env:PATH
    } else {
        Log "WARN: clang-cl が無いので MSVC を使う"
    }
}
foreach ($d in $BuildDir, $StageRoot, $InstallDir) {
    if (Test-Path $d) { Remove-Item $d -Recurse -Force }
}
New-Item -ItemType Directory -Force $PkgDir | Out-Null
Build-And-Install
Assemble-Package
Make-Zip
Verify-Zip
Get-Item $ZipPath | Format-List Name, Length
