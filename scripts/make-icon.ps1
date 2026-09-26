# Generates build/app.ico — the application icon, drawn in code rather than
# checked in as a binary, so the artwork is reviewable and editable like
# everything else here.
#
# The mark is the same object as the tray icon with room for detail: the
# TOSLINK socket (square, top corners chamfered — the keying that stops a plug
# going in upside down), a metallic ferrule inside it, and the red light
# inside that. The socket corners round at large sizes, where sharp ones look
# cheap; the ferrule is drawn larger than scale realism would suggest, because
# a true-to-life ferrule is a quarter of the socket's width and disappears in
# a file listing.
#
# The .ico is assembled by hand from PNG-compressed entries, which every
# Windows since Vista reads.

param([string]$OutputPath = "$PSScriptRoot\..\build\app.ico")

Add-Type -AssemblyName System.Drawing

function Draw-Icon([int]$size) {
    $bitmap = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

    $s = [float]$size
    $stroke = [Math]::Max(1.0, $s * 0.06)

    # The connector face: wider than tall, with *small* 45-degree chamfers on
    # the top corners — the TOSLINK keying. The chamfer size is the whole
    # likeness: at 30% of the width the top edge all but vanished and the
    # outline read as a pentagon; at 18% it reads as a plug face again.
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $left = $s * 0.06; $right = $s - $s * 0.06
    $top = $s * 0.15; $bottom = $s - $s * 0.11
    $chamfer = $s * 0.18
    $points = @(
        (New-Object System.Drawing.PointF(($left + $chamfer), $top)),
        (New-Object System.Drawing.PointF(($right - $chamfer), $top)),
        (New-Object System.Drawing.PointF($right, ($top + $chamfer))),
        (New-Object System.Drawing.PointF($right, $bottom)),
        (New-Object System.Drawing.PointF($left, $bottom)),
        (New-Object System.Drawing.PointF($left, ($top + $chamfer)))
    )
    $path.AddPolygon($points)

    $body = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 38, 42, 48))
    $g.FillPath($body, $path)
    $edge = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 120, 128, 138), $stroke)
    $edge.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawPath($edge, $path)

    # The ferrule: a metal ring, centred in the face.
    $cx = $s / 2; $cy = ($top + $bottom) / 2 + $s * 0.02
    $ferruleR = $s * 0.21
    $ferrule = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 176, 182, 190))
    $g.FillEllipse($ferrule, ($cx - $ferruleR), ($cy - $ferruleR), ($ferruleR * 2), ($ferruleR * 2))

    # The red light inside it — the LED you can see glowing in a live
    # connector, and the one spot of colour in the mark.
    $ledR = $s * 0.13
    $led = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 226, 48, 44))
    $g.FillEllipse($led, ($cx - $ledR), ($cy - $ledR), ($ledR * 2), ($ledR * 2))

    $g.Dispose()
    return $bitmap
}

$sizes = @(256, 64, 48, 32, 16)
$pngs = @()
foreach ($size in $sizes) {
    $bitmap = Draw-Icon $size
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += ,@{ Size = $size; Bytes = $stream.ToArray() }
    $stream.Dispose(); $bitmap.Dispose()
}

$outputDirectory = Split-Path -Parent $OutputPath
if (-not (Test-Path $outputDirectory)) {
    New-Item -ItemType Directory -Force $outputDirectory | Out-Null
}
$out = New-Object System.IO.BinaryWriter([System.IO.File]::Create($OutputPath))
$out.Write([UInt16]0)              # reserved
$out.Write([UInt16]1)              # type: icon
$out.Write([UInt16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($entry in $pngs) {
    $dimension = if ($entry.Size -ge 256) { 0 } else { $entry.Size }
    $out.Write([Byte]$dimension)   # width (0 = 256)
    $out.Write([Byte]$dimension)   # height
    $out.Write([Byte]0)            # palette
    $out.Write([Byte]0)            # reserved
    $out.Write([UInt16]1)          # planes
    $out.Write([UInt16]32)         # bpp
    $out.Write([UInt32]$entry.Bytes.Length)
    $out.Write([UInt32]$offset)
    $offset += $entry.Bytes.Length
}
foreach ($entry in $pngs) { $out.Write($entry.Bytes) }
$out.Close()
Write-Host "wrote $OutputPath ($($pngs.Count) sizes)"
