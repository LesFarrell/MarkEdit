Add-Type -AssemblyName System.Drawing

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $path = $null
    $brush = $null
    $pen = $null
    $font = $null
    try {
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $g.Clear([System.Drawing.Color]::Transparent)

        $margin = [Math]::Max(1, [int]($size * 0.06))
        $rect = New-Object System.Drawing.Rectangle $margin, $margin, ($size - 2*$margin), ($size - 2*$margin)
        $radius = [Math]::Max(2, [int]($size * 0.16))

        # rounded-rect background
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $d = $radius * 2
        $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
        $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
        $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
        $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
        $path.CloseFigure()

        $bg1 = [System.Drawing.Color]::FromArgb(255, 37, 99, 235)
        $bg2 = [System.Drawing.Color]::FromArgb(255, 29, 60, 150)
        $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $bg1, $bg2, 90.0)
        $g.FillPath($brush, $path)

        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 15, 35, 90)), ([Math]::Max(1, $size * 0.02))
        $g.DrawPath($pen, $path)

        # bold "T" glyph, nudged up-left a touch to leave room for the bolt accent
        $fontSize = $size * 0.50
        $font = New-Object System.Drawing.Font "Segoe UI", $fontSize, ([System.Drawing.FontStyle]::Bold)
        $textBrush = [System.Drawing.Brushes]::White
        $tFormat = New-Object System.Drawing.StringFormat
        $tFormat.Alignment = [System.Drawing.StringAlignment]::Center
        $tFormat.LineAlignment = [System.Drawing.StringAlignment]::Center
        $tRect = New-Object System.Drawing.RectangleF (($size * -0.08)), ($size * -0.08), $size, $size
        $g.DrawString("T", $font, $textBrush, $tRect, $tFormat)

        # small hex-nut accent, bottom-right, evoking "tinker" / tools
        $cx = $size * 0.745
        $cy = $size * 0.745
        $hr = $size * 0.155
        $hexPts = New-Object System.Collections.Generic.List[System.Drawing.PointF]
        for ($i = 0; $i -lt 6; $i++) {
            $angle = (60 * $i - 90) * [Math]::PI / 180.0
            $hexPts.Add((New-Object System.Drawing.PointF (($cx + $hr * [Math]::Cos($angle))), ($cy + $hr * [Math]::Sin($angle))))
        }
        $g.FillPolygon([System.Drawing.Brushes]::White, $hexPts.ToArray())
        $holeBrush = New-Object System.Drawing.SolidBrush $bg2
        $holeR = $hr * 0.42
        $g.FillEllipse($holeBrush, ($cx - $holeR), ($cy - $holeR), ($holeR * 2), ($holeR * 2))
        $holeBrush.Dispose()
    }
    finally {
        $g.Dispose()
        if ($font) { $font.Dispose() }
        if ($brush) { $brush.Dispose() }
        if ($pen) { $pen.Dispose() }
        if ($path) { $path.Dispose() }
    }
    return $bmp
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$bitmaps = @()
foreach ($s in $sizes) { $bitmaps += New-IconBitmap $s }

$icoPath = Join-Path $PSScriptRoot "tinkeredit.ico"
$fs = New-Object System.IO.FileStream $icoPath, ([System.IO.FileMode]::Create)
try {
    $bw = New-Object System.IO.BinaryWriter $fs
    try {
        # ICONDIR
        $bw.Write([UInt16]0)      # reserved
        $bw.Write([UInt16]1)      # type = icon
        $bw.Write([UInt16]$bitmaps.Count)

        $pngDatas = @()
        foreach ($bmp in $bitmaps) {
            $ms = New-Object System.IO.MemoryStream
            $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
            $pngDatas += ,($ms.ToArray())
        }

        $offset = 6 + (16 * $bitmaps.Count)
        for ($i = 0; $i -lt $bitmaps.Count; $i++) {
            $s = $sizes[$i]
            $data = $pngDatas[$i]
            $bw.Write([byte]($(if ($s -ge 256) {0} else {$s})))   # width (0 = 256)
            $bw.Write([byte]($(if ($s -ge 256) {0} else {$s})))   # height
            $bw.Write([byte]0)    # color palette
            $bw.Write([byte]0)    # reserved
            $bw.Write([UInt16]1)  # color planes
            $bw.Write([UInt16]32) # bits per pixel
            $bw.Write([UInt32]$data.Length)
            $bw.Write([UInt32]$offset)
            $offset += $data.Length
        }
        foreach ($data in $pngDatas) {
            $bw.Write($data)
        }
        $bw.Flush()
    }
    finally {
        $bw.Close()
    }
}
finally {
    $fs.Close()
    foreach ($bmp in $bitmaps) { $bmp.Dispose() }
}

Write-Output "Icon written to $icoPath"
