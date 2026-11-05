param([Parameter(Mandatory)][string]$Source, [string]$Out = "$PSScriptRoot\..\assets\icon")
# The app's icon on every system, made from one square-ish picture with transparent corners (the owner's "SO" icon):
#   icon.png       1024 x 1024, the master (Linux's desktop entry, README, stores)
#   icon-256.png   built into the window (its title bar, taskbar, macOS's Dock)
#   spikeout.ico   Windows: the .exe's icon (16-256, PNG inside)
#   spikeout.icns  macOS: an app bundle's icon (16-1024, PNG inside)
# Run again when the picture changes: tools\make-icons.ps1 -Source "C:\path\to\icon.png"
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$src = [System.Drawing.Image]::FromFile((Resolve-Path $Source).Path)
# (centred on a square canvas: a picture a pixel off square isn't stretched)
$side = [Math]::Max($src.Width, $src.Height)
$square = New-Object System.Drawing.Bitmap $side, $side, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($square)
$g.Clear([System.Drawing.Color]::Transparent)
$g.DrawImage($src, [int](($side - $src.Width) / 2), [int](($side - $src.Height) / 2), $src.Width, $src.Height)
$g.Dispose(); $src.Dispose()

function png([int]$size) {
    $b = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.CompositingQuality = 'HighQuality'
    $g.SmoothingMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $attr = New-Object System.Drawing.Imaging.ImageAttributes
    $attr.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY) # (no dark fringe at the edges)
    $g.DrawImage($square, (New-Object System.Drawing.Rectangle 0, 0, $size, $size), 0, 0, $side, $side, 'Pixel', $attr)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $b.Dispose()
    return , $ms.ToArray()
}
function be32([System.IO.BinaryWriter]$w, [uint32]$v) { $w.Write([byte](($v -shr 24) -band 255)); $w.Write([byte](($v -shr 16) -band 255)); $w.Write([byte](($v -shr 8) -band 255)); $w.Write([byte]($v -band 255)) }

[IO.File]::WriteAllBytes("$Out\icon.png", (png 1024))
[IO.File]::WriteAllBytes("$Out\icon-256.png", (png 256))

# Windows .ico: a directory of PNG images (Vista and later read PNG at every size)
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$images = foreach ($s in $sizes) { , (png $s) }
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ms
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $images[$i].Length
    $w.Write([byte]($s % 256)); $w.Write([byte]($s % 256)) # (256 is written as 0)
    $w.Write([byte]0); $w.Write([byte]0); $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$len); $w.Write([uint32]$offset)
    $offset += $len
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush(); [IO.File]::WriteAllBytes("$Out\spikeout.ico", $ms.ToArray())

# macOS .icns: big-endian chunks, PNG inside each
$types = [ordered]@{ icp4 = 16; icp5 = 32; icp6 = 64; ic07 = 128; ic08 = 256; ic09 = 512; ic10 = 1024 }
$body = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $body
foreach ($t in $types.Keys) {
    $img = png $types[$t]
    $bw.Write([Text.Encoding]::ASCII.GetBytes($t)); be32 $bw (8 + $img.Length); $bw.Write($img)
}
$bw.Flush()
$ms = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $ms
$w.Write([Text.Encoding]::ASCII.GetBytes("icns")); be32 $w (8 + $body.Length); $w.Write($body.ToArray())
$w.Flush(); [IO.File]::WriteAllBytes("$Out\spikeout.icns", $ms.ToArray())
$square.Dispose()
Get-ChildItem $Out | ForEach-Object { "{0,-16} {1,9} bytes" -f $_.Name, $_.Length }
