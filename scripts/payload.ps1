# Dot-source this, then call Get-ReleasePayload: the files a release ships,
# defined once so the portable zip and the installer cannot disagree about
# what the app needs to run. Throws if any is missing from build\, because a
# release that silently depends on something the target machine happens to
# have is the works-on-my-machine artifact these scripts exist to prevent.

function Get-ReleasePayload([string]$build) {
    $fixed = @('AC3SPDIFApp.exe', 'ac3spdif.exe', 'audioprobe.exe',
               'FFMPEG-NOTICE.md', 'LGPL-2.1.txt', 'LICENSE')
    foreach ($file in $fixed) {
        if (-not (Test-Path (Join-Path $build $file))) {
            throw "build\$file is missing - run scripts\build-ffmpeg.ps1 then scripts\build.ps1 first"
        }
    }
    # The binaries link these four by name; one missing is a program that
    # will not start.
    $dlls = @()
    foreach ($library in 'avcodec', 'avformat', 'avutil', 'swresample') {
        $found = Get-ChildItem $build -Filter "$library-*.dll" | Select-Object -First 1
        if (-not $found) {
            throw "build\$library-*.dll is missing - run scripts\build-ffmpeg.ps1 then scripts\build.ps1"
        }
        $dlls += $found.Name
    }
    return $fixed + $dlls
}
