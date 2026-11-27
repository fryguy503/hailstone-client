[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $SourceRoot,
    [Parameter(Mandatory)]
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$outputPath = (Resolve-Path -LiteralPath $OutputDirectory).ProviderPath

$width = 328
$height = 628
$portrait = [System.Drawing.Bitmap]::new($width, $height)
$canvas = [System.Drawing.Graphics]::FromImage($portrait)
$landscape = [System.Drawing.Image]::FromFile((Join-Path $sourceRoot 'pictures/hailstone.jpg'))
$crest = [System.Drawing.Image]::FromFile((Join-Path $sourceRoot 'icons/hailstone-crest.png'))
$bounds = [System.Drawing.Rectangle]::new(0, 0, $width, $height)
$fade = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
    $bounds, [System.Drawing.Color]::Black, [System.Drawing.Color]::Transparent, 90.0
)
$edge = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(155, 146, 184, 216), 1.0)
$outerEdge = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(225, 12, 18, 37), 5.0)

try {
    $canvas.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $canvas.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    # The eastern crop keeps the fortress and leaves the landscape's baked-in wordmark out.
    $fortress = [System.Drawing.Rectangle]::new(658, 0, 292, 559)
    $canvas.DrawImage($landscape, $bounds, $fortress, [System.Drawing.GraphicsUnit]::Pixel)

    $shading = [System.Drawing.Drawing2D.ColorBlend]::new()
    $shading.Positions = [single[]] @(0.0, 0.32, 0.54, 0.88, 1.0)
    $shading.Colors = [System.Drawing.Color[]] @(
        [System.Drawing.Color]::FromArgb(245, 13, 17, 34),
        [System.Drawing.Color]::FromArgb(212, 17, 21, 44),
        [System.Drawing.Color]::FromArgb(16, 26, 33, 61),
        [System.Drawing.Color]::FromArgb(12, 22, 27, 50),
        [System.Drawing.Color]::FromArgb(200, 12, 18, 35)
    )
    $fade.InterpolationColors = $shading
    $canvas.FillRectangle($fade, $bounds)

    $canvas.DrawImage($crest, [System.Drawing.Rectangle]::new(28, 28, 272, 272))
    $canvas.DrawRectangle($outerEdge, 2, 2, ($width - 5), ($height - 5))
    $canvas.DrawRectangle($edge, 7, 7, ($width - 15), ($height - 15))
    $portrait.Save((Join-Path $outputPath 'WizardImage.png'), [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $outerEdge.Dispose()
    $edge.Dispose()
    $fade.Dispose()
    $crest.Dispose()
    $landscape.Dispose()
    $canvas.Dispose()
    $portrait.Dispose()
}

$small = [System.Drawing.Bitmap]::new(110, 110)
$canvas = [System.Drawing.Graphics]::FromImage($small)
$icon = [System.Drawing.Image]::FromFile((Join-Path $sourceRoot 'icons/hailstone.png'))

try {
    $canvas.Clear([System.Drawing.Color]::Transparent)
    $canvas.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $canvas.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $canvas.DrawImage($icon, [System.Drawing.Rectangle]::new(4, 4, 102, 102))
    $small.Save((Join-Path $outputPath 'WizardSmallImage.png'), [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $icon.Dispose()
    $canvas.Dispose()
    $small.Dispose()
}
