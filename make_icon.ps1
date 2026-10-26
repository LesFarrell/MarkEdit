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

        # rounded-rect document background
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $d = $radius * 2
        $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
        $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
        $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
        $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
        $path.CloseFigure()

        $bg1 = [System.Drawing.Color]::FromArgb(255, 41, 98, 189)
        $bg2 = [System.Drawing.Color]::FromArgb(255, 27, 66, 138)
        $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $bg1, $bg2, 90.0)
        $g.FillPath($brush, $path)

        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 20, 48, 100)), ([Math]::Max(1, $size * 0.02))
        $g.DrawPath($pen, $path)

        # "M" glyph plus a small down-arrow to evoke markdown / live preview
        $fontSize = $size * 0.46
        $font = New-Object System.Drawing.Font "Segoe UI", $fontSize, ([System.Drawing.FontStyle]::Bold)
        $textBrush = [System.Drawing.Brushes]::White
        $mFormat = New-Object System.Drawing.StringFormat
        $mFormat.Alignment = [System.Drawing.StringAlignment]::Center
        $mFormat.LineAlignment = [System.Drawing.StringAlignment]::Center
        $mRect = New-Object System.Drawing.RectangleF 0, ($size * -0.06), $size, $size
        $g.DrawString("M", $font, $textBrush, $mRect, $mFormat)

        # small arrow/caret at bottom right suggesting "preview / down"
        $arrowSize = $size * 0.20
        $ax = $size * 0.66
        $ay = $size * 0.64
        $arrowPts = @(
            (New-Object System.Drawing.PointF ($ax), ($ay)),
            (New-Object System.Drawing.PointF ($ax + $arrowSize), ($ay)),
            (New-Object System.Drawing.PointF ($ax + $arrowSize/2), ($ay + $arrowSize*0.85))
        )
        $g.FillPolygon([System.Drawing.Brushes]::White, $arrowPts)
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

$icoPath = Join-Path $PSScriptRoot "markedit.ico"
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
