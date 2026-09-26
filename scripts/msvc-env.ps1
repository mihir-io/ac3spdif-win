# Dot-source this, then call Import-MsvcEnvironment: finds the newest Visual
# Studio (Build Tools are enough) through vswhere and imports its x64
# developer environment into the current PowerShell process, so cl, link, lib
# and dumpbin resolve without a Developer Command Prompt.

function Import-MsvcEnvironment {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe not found - install Visual Studio 2022 Build Tools with the C++ workload'
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'no Visual Studio installation with the C++ toolset was found' }

    $devCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
    $envDump = cmd /s /c "`"$devCmd`" -arch=x64 -host_arch=x64 -no_logo && set"
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
        throw 'cl.exe still not available after VsDevCmd'
    }
}
