# Build the minimal shared FFmpeg the engine links against, into vendor\ffmpeg:
#
#   vendor\ffmpeg\bin\      avcodec-*.dll avformat-*.dll avutil-*.dll swresample-*.dll
#   vendor\ffmpeg\include\  public headers
#   vendor\ffmpeg\lib\      MSVC import libraries (avcodec.lib ...)
#
# The engine encodes in-process, on the output device's render thread — the
# placement that removes the encoded-burst ring from the latency budget — so it
# needs FFmpeg as libraries rather than as ffmpeg.exe. Exactly four of them,
# with everything else switched off:
#
#   libavcodec     the AC-3 and DTS encoders (and their decoders, which only
#                  the test harness uses to prove the output decodes back)
#   libavformat    the IEC 61937 "spdif" muxer (and demuxer, for the tests)
#   libswresample  mix format -> encoder format, layout and rate
#   libavutil      what the other three stand on
#
# ffmpeg's build system needs a POSIX shell even on Windows, so this drives a
# MSYS2/MinGW-w64 toolchain (winget install MSYS2.MSYS2). MinGW emits GNU import
# libraries, so the MSVC ones are generated here from each DLL's own export
# table — dumpbin to list them, lib to wrap them — which is what lets the
# MSVC-built engine link against a MinGW-built FFmpeg.
#
# Licensing: LGPL-2.1+. No --enable-gpl, no --enable-nonfree. The engine links
# the DLLs dynamically and they ship as separate, replaceable files beside it,
# which is the LGPL's own condition for linking. vendor\FFMPEG-NOTICE.md, which
# this script writes with the exact version and flags, travels with them.

param(
    [string]$Version = $(if ($env:FFMPEG_VERSION) { $env:FFMPEG_VERSION } else { '9.0.2' }),
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vendor = Join-Path $root 'vendor'
$prefix = Join-Path $vendor 'ffmpeg'

if ((Test-Path (Join-Path $prefix 'lib\avcodec.lib')) -and -not $Force) {
    Write-Host 'vendor\ffmpeg already built; use -Force to rebuild'
    exit 0
}

$msys = 'C:\msys64'
if (-not (Test-Path "$msys\usr\bin\bash.exe")) {
    throw 'MSYS2 not found at C:\msys64 - run: winget install MSYS2.MSYS2'
}

function Invoke-Mingw([string]$command) {
    $env:MSYSTEM = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    & "$msys\usr\bin\bash.exe" -lc $command
    if ($LASTEXITCODE -ne 0) { throw "msys2 command failed: $command" }
}

function ConvertTo-MsysPath([string]$path) {
    ($path -replace '\\', '/') -replace '^([A-Za-z]):', '/$1'
}

Write-Host '==> Ensuring toolchain packages'
Invoke-Mingw 'pacman -S --noconfirm --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-nasm make diffutils 2>&1 | tail -2'

Write-Host "==> Fetching ffmpeg $Version"
New-Item -ItemType Directory -Force $vendor | Out-Null
$work = Join-Path $vendor '.build-ffmpeg'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
if (Test-Path $prefix) { Remove-Item -Recurse -Force $prefix }
New-Item -ItemType Directory -Force $work | Out-Null
$workPosix = ConvertTo-MsysPath $work
$prefixPosix = ConvertTo-MsysPath $prefix
# Built where tar puts it, not renamed: renaming a directory of several
# thousand files the moment they land races the real-time virus scanner,
# which still holds handles on them, and fails with "Permission denied".
Invoke-Mingw "cd '$workPosix' && curl -fsSL --max-time 600 -o ffmpeg.tar.xz https://ffmpeg.org/releases/ffmpeg-$Version.tar.xz && tar xf ffmpeg.tar.xz"
$sourcePosix = "$workPosix/ffmpeg-$Version"

# -static folds the MinGW runtime into each DLL. w32threads alone is not
# enough: avutil still pulls libwinpthread-1.dll in for clock_gettime and
# nanosleep, which mingw-w64 implements there. The dependency check below
# enforces the result, because a DLL that needs a runtime nobody ships loads
# fine here and fails on the next machine.
$configureFlags = @(
    "--prefix=$prefixPosix",
    '--enable-shared', '--disable-static',
    '--disable-programs', '--disable-doc', '--disable-debug',
    '--disable-avdevice', '--disable-avfilter', '--disable-swscale',
    '--disable-network', '--disable-autodetect',
    '--enable-w32threads',
    '--extra-ldflags=-static',
    '--disable-everything',
    '--enable-encoder=ac3,dca',
    '--enable-decoder=ac3,dca',
    '--enable-muxer=spdif',
    '--enable-demuxer=spdif',
    '--enable-parser=ac3,dca',
    '--enable-protocol=file'
)
$flagString = $configureFlags -join ' '

Write-Host '==> Configuring'
Invoke-Mingw "cd '$sourcePosix' && ./configure $flagString > configure.log 2>&1 || { echo 'configure failed:'; tail -25 configure.log; exit 1; }"

Write-Host '==> Building'
Invoke-Mingw "cd '$sourcePosix' && make -j`$(nproc) > build.log 2>&1 && make install > install.log 2>&1 || { echo 'build failed:'; tail -30 build.log; tail -10 install.log; exit 1; }"

Write-Host '==> Generating MSVC import libraries'
. (Join-Path $PSScriptRoot 'msvc-env.ps1')
Import-MsvcEnvironment
$libDir = Join-Path $prefix 'lib'
$systemDlls = @('kernel32.dll', 'msvcrt.dll', 'bcrypt.dll', 'user32.dll', 'advapi32.dll',
                'ole32.dll', 'shell32.dll', 'ws2_32.dll', 'secur32.dll', 'psapi.dll')
foreach ($dll in Get-ChildItem (Join-Path $prefix 'bin') -Filter '*.dll') {
    $base = $dll.BaseName -replace '-\d+$', ''
    $exports = foreach ($line in (& dumpbin /nologo /exports $dll.FullName)) {
        if ($line -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { "    $($matches[1])" }
    }
    $def = Join-Path $libDir "$base.def"
    (@("LIBRARY $($dll.Name)", 'EXPORTS') + $exports) | Set-Content -Encoding ascii $def
    & lib /nologo /machine:x64 "/def:$def" ("/out:" + (Join-Path $libDir "$base.lib")) | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib.exe failed for $($dll.Name)" }

    $deps = foreach ($line in (& dumpbin /nologo /dependents $dll.FullName)) {
        if ($line -match '^\s+(\S+\.dll)\s*$') { $matches[1].ToLower() }
    }
    $foreign = $deps | Where-Object { $_ -notin $systemDlls -and $_ -notmatch '^(avcodec|avformat|avutil|swresample)-\d+\.dll$' -and $_ -notmatch '^api-ms-win-' }
    if ($foreign) { throw "$($dll.Name) depends on non-system DLLs: $($foreign -join ', ')" }
    Write-Host "    $($dll.Name) -> $base.lib ($($exports.Count) exports)"
}

@"
# Bundled FFmpeg

The ``avcodec``, ``avformat``, ``avutil`` and ``swresample`` DLLs shipped beside
the binaries are a minimal shared build of **FFmpeg $Version**, compiled by
``scripts\build-ffmpeg.ps1`` with exactly these options:

``````
./configure $flagString
``````

FFmpeg is licensed under the **LGPL-2.1-or-later** in this configuration (no
``--enable-gpl``, no ``--enable-nonfree``). The ac3spdif binaries link these
DLLs dynamically; they are separate files, and you may replace them with any
ABI-compatible build of the same FFmpeg major versions. This project's MIT
licence does not apply to FFmpeg.

Corresponding source: https://ffmpeg.org/releases/ffmpeg-$Version.tar.xz

Ship this notice (and ``LGPL-2.1.txt``) with any redistribution of the DLLs.
"@ | Out-File -Encoding utf8 (Join-Path $vendor 'FFMPEG-NOTICE.md')

# The static ffmpeg.exe from the subprocess era is not used by anything now.
$legacy = Join-Path $vendor 'ffmpeg.exe'
if (Test-Path $legacy) { Remove-Item $legacy }

Remove-Item -Recurse -Force $work
Write-Host "==> Done: $prefix"
Get-ChildItem (Join-Path $prefix 'bin') -Filter '*.dll' | ForEach-Object {
    Write-Host ("    {0,-24} {1,6:N0} KB" -f $_.Name, ($_.Length / 1KB))
}
