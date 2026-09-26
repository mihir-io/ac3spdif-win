# Compiles installer\ac3spdif.iss into dist\ac3spdif-setup-<version>.exe with
# Inno Setup ( winget install JRSoftware.InnoSetup ). The version comes out of
# Version.h, the payload out of build\ — so the installer cannot ship anything
# other than what the build scripts produced, and refuses to build at all if
# any of it is missing (see payload.ps1, which the portable zip shares).

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'

$iscc = @("$env:ProgramFiles (x86)\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
          "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'ISCC.exe not found - run: winget install JRSoftware.InnoSetup' }

. (Join-Path $PSScriptRoot 'payload.ps1')
Get-ReleasePayload $build | Out-Null

$versionLine = Select-String -Path (Join-Path $root 'src\engine\Version.h') -Pattern 'kVersion = "([^"]+)"'
$version = $versionLine.Matches[0].Groups[1].Value
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null

& $iscc /Qp "/DAppVersion=$version" "/DSourceDir=$build" "/DOutputDir=$dist" (Join-Path $root 'installer\ac3spdif.iss')
if ($LASTEXITCODE -ne 0) { throw 'ISCC failed' }

$setup = Join-Path $dist "ac3spdif-setup-$version.exe"
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
"$hash  $(Split-Path -Leaf $setup)" | Out-File -Encoding ascii "$setup.sha256"
Write-Host "built    $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
Write-Host "sha256   $hash"
