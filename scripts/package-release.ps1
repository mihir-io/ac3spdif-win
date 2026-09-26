# Packages build\ into dist\ac3spdif-win-<version>.zip with a SHA-256, ready
# to attach to a release. The zip *is* the installation: the binaries link the
# CRT statically, the FFmpeg DLLs travel beside them, settings live in HKCU,
# and nothing registers anything — unzip anywhere and run.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'

$versionLine = Select-String -Path (Join-Path $root 'src\engine\Version.h') -Pattern 'kVersion = "([^"]+)"'
$version = $versionLine.Matches[0].Groups[1].Value

. (Join-Path $PSScriptRoot 'payload.ps1')
$required = Get-ReleasePayload $build

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
$zip = Join-Path $dist "ac3spdif-win-$version.zip"
if (Test-Path $zip) { Remove-Item $zip }

$staging = Join-Path $dist '.staging'
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force $staging | Out-Null
foreach ($file in $required) { Copy-Item (Join-Path $build $file) $staging }
Compress-Archive -Path "$staging\*" -DestinationPath $zip
Remove-Item -Recurse -Force $staging

# Unpack our own output and verify the payload survived, because an archive
# that only ever worked in the directory it was created from is a failure
# that shows up on someone else's machine.
$check = Join-Path $dist '.verify'
if (Test-Path $check) { Remove-Item -Recurse -Force $check }
Expand-Archive $zip $check
foreach ($file in $required) {
    $a = (Get-FileHash (Join-Path $build $file)).Hash
    $b = (Get-FileHash (Join-Path $check $file)).Hash
    if ($a -ne $b) { throw "$file did not survive the archive round-trip" }
}
Remove-Item -Recurse -Force $check

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path -Leaf $zip)" | Out-File -Encoding ascii "$zip.sha256"
Write-Host "packaged $zip"
Write-Host "sha256   $hash"
