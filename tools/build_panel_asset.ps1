# Build the static VisualTFT card from vector geometry using Windows GDI+.
# Re-run after changing Radius; no MCU code or screen scripts are involved.
param([ValidateRange(1, 86)][int]$Radius = 20)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$panelWidth = 944
$panelHeight = 172
$scale = 4
$assetDirectory = Join-Path $PSScriptRoot '..\VisualTFT\Project\images'
New-Item -ItemType Directory -Path $assetDirectory -Force | Out-Null
$assetPath = Join-Path $assetDirectory 'MaintenancePanel.png'
$canvas = [System.Drawing.Bitmap]::new($panelWidth * $scale, $panelHeight * $scale)
$graphics = [System.Drawing.Graphics]::FromImage($canvas)
$path = [System.Drawing.Drawing2D.GraphicsPath]::new()
$imageAttributes = [System.Drawing.Imaging.ImageAttributes]::new()
$output = $null
$outputGraphics = $null
try {
    $graphics.Clear([System.Drawing.Color]::FromArgb(237, 241, 244))
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $diameter = 2 * $Radius * $scale
    $right = $panelWidth * $scale - $diameter
    $bottom = $panelHeight * $scale - $diameter
    $path.AddArc(0, 0, $diameter, $diameter, 180, 90)
    $path.AddArc($right, 0, $diameter, $diameter, 270, 90)
    $path.AddArc($right, $bottom, $diameter, $diameter, 0, 90)
    $path.AddArc(0, $bottom, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    $graphics.FillPath([System.Drawing.Brushes]::White, $path)
    $output = [System.Drawing.Bitmap]::new($panelWidth, $panelHeight,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $outputGraphics = [System.Drawing.Graphics]::FromImage($output)
    $outputGraphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $outputGraphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    # Mirror edge pixels when resampling to avoid a dark border at the canvas edge.
    $imageAttributes.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)
    $outputGraphics.DrawImage($canvas, [System.Drawing.Rectangle]::new(0, 0, $panelWidth, $panelHeight),
        0, 0, $canvas.Width, $canvas.Height, [System.Drawing.GraphicsUnit]::Pixel, $imageAttributes)
    $output.Save($assetPath, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "Created $assetPath ($panelWidth x $panelHeight, radius $Radius px)"
}
finally {
    if ($null -ne $outputGraphics) { $outputGraphics.Dispose() }
    if ($null -ne $output) { $output.Dispose() }
    $path.Dispose()
    $imageAttributes.Dispose()
    $graphics.Dispose()
    $canvas.Dispose()
}
