# Builds everything into build\ with nothing but the Visual Studio Build Tools
# installed — no CMake, no MSBuild project files, the same "buildable with the
# command-line tools alone" bargain the macOS version makes with its shell
# script. Run from anywhere:
#
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1
#
# Needs vendor\ffmpeg (scripts\build-ffmpeg.ps1, once). Produces:
#   build\AC3SPDIFApp.exe      the tray app (also the --service host)
#   build\ac3spdif.exe         the CLI
#   build\audioprobe.exe       the device prober
#   build\ac3spdif_tests.exe   the hardware-free tests (not shipped)
#   build\*.dll                the four FFmpeg libraries, beside the binaries
#
# The app is not named AC3SPDIF.exe for a reason worth recording: Windows
# filesystems are case-insensitive, so AC3SPDIF.exe and ac3spdif.exe are the
# same file, and the second link quietly replaces the first.
#
# The binaries link the CRT statically, so the only runtime files they need
# are the FFmpeg DLLs copied next to them — which is also where the LGPL wants
# them: separate, and replaceable.

param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$ffmpeg = Join-Path $root 'vendor\ffmpeg'

if (-not (Test-Path (Join-Path $ffmpeg 'lib\avcodec.lib'))) {
    throw 'vendor\ffmpeg is missing - run scripts\build-ffmpeg.ps1 first (needs MSYS2)'
}

. (Join-Path $PSScriptRoot 'msvc-env.ps1')
Import-MsvcEnvironment

# --- Version: single source of truth is Version.h. ---------------------------
$versionLine = Select-String -Path (Join-Path $root 'src\engine\Version.h') -Pattern 'kVersion = "([^"]+)"'
$version = $versionLine.Matches[0].Groups[1].Value
$versionCommas = (($version -split '\.') + @('0', '0', '0'))[0..2] -join ','
$versionCommas = "$versionCommas,0"
Write-Host "building ac3spdif $version"

New-Item -ItemType Directory -Force $build | Out-Null

# --- Icon and resources. ------------------------------------------------------
$icon = Join-Path $build 'app.ico'
if (-not (Test-Path $icon)) {
    & (Join-Path $PSScriptRoot 'make-icon.ps1') -OutputPath $icon
}
$res = Join-Path $build 'app.res'
& rc /nologo /i $build /d "APP_VERSION_STR=\`"$version\`"" /d "APP_VERSION_COMMAS=$versionCommas" /fo $res (Join-Path $root 'src\app\app.rc')
if ($LASTEXITCODE -ne 0) { throw 'rc failed' }

# --- Compile and link. --------------------------------------------------------
$commonSources = @(
    'src\support\AudioDevices.cpp',
    'src\engine\Config.cpp',
    'src\engine\Capture.cpp',
    'src\engine\Encoder.cpp',
    'src\engine\BitstreamOutput.cpp',
    'src\engine\Engine.cpp'
)
$libraries = @('ole32.lib', 'uuid.lib', 'avrt.lib', 'winmm.lib', 'user32.lib',
               'gdi32.lib', 'advapi32.lib', 'shell32.lib', 'gdiplus.lib',
               'avcodec.lib', 'avformat.lib', 'avutil.lib', 'swresample.lib')
# FFmpeg's headers are C written for GCC and Clang; /external keeps /W4 aimed
# at this project's code rather than at theirs.
$compileFlags = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT',
                  '/utf-8', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX',
                  '/external:W0', ('/external:I' + (Join-Path $ffmpeg 'include')))

function Build-Target([string]$name, [string[]]$sources, [string]$subsystem, [string[]]$extraLink) {
    $objDir = Join-Path $build "obj\$name"
    if (Test-Path $objDir) { Remove-Item -Recurse -Force $objDir }
    New-Item -ItemType Directory -Force $objDir | Out-Null
    $sourcePaths = $sources | ForEach-Object { Join-Path $root $_ }
    & cl @compileFlags /c ("/Fo$objDir\") @sourcePaths
    if ($LASTEXITCODE -ne 0) { throw "compile failed for $name" }
    $objects = Get-ChildItem $objDir -Filter '*.obj' | ForEach-Object { $_.FullName }
    $output = Join-Path $build $name
    & link /nologo ("/SUBSYSTEM:$subsystem") ("/OUT:$output") ('/LIBPATH:' + (Join-Path $ffmpeg 'lib')) @objects @libraries @extraLink
    if ($LASTEXITCODE -ne 0) { throw "link failed for $name" }
    Write-Host "built $output"
}

Build-Target 'ac3spdif.exe' ($commonSources + @('src\cli\main.cpp', 'src\cli\CommandLine.cpp')) 'CONSOLE' @()
Build-Target 'audioprobe.exe' ($commonSources + @('src\audioprobe\main.cpp')) 'CONSOLE' @()
Build-Target 'AC3SPDIFApp.exe' ($commonSources + @(
    'src\app\main.cpp', 'src\app\Settings.cpp', 'src\app\StatusIcon.cpp',
    'src\app\StatusWindow.cpp', 'src\app\ServiceMode.cpp')) 'WINDOWS' @(
    $res, '/MANIFEST:EMBED', ('/MANIFESTINPUT:' + (Join-Path $root 'src\app\app.manifest')))
Build-Target 'ac3spdif_tests.exe' ($commonSources + @('src\tests\main.cpp')) 'CONSOLE' @()

# The FFmpeg DLLs beside the binaries, and the licence notices with them,
# because the obligation is for the notices to accompany the libraries rather
# than merely to exist in the repository they were built from.
Get-ChildItem $build -Filter '*.dll' | Remove-Item
Get-ChildItem (Join-Path $ffmpeg 'bin') -Filter '*.dll' | Copy-Item -Destination $build
$legacyExe = Join-Path $build 'ffmpeg.exe'
if (Test-Path $legacyExe) { Remove-Item $legacyExe }
foreach ($notice in 'FFMPEG-NOTICE.md', 'LGPL-2.1.txt') {
    Copy-Item (Join-Path $root "vendor\$notice") $build -Force
}
Copy-Item (Join-Path $root 'LICENSE') $build -Force
Write-Host 'bundled the FFmpeg DLLs (+ licence notices)'

if (-not $SkipTests) {
    Write-Host "`nrunning tests"
    & (Join-Path $build 'ac3spdif_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'tests failed' }
}

Write-Host "`ndone. binaries are in build\"
