param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not [System.IO.File]::Exists($InputPath)) {
    throw "Icon source was not found: $InputPath"
}

$directory = [System.IO.Path]::GetDirectoryName($OutputPath)
[System.IO.Directory]::CreateDirectory($directory) | Out-Null

$source = [System.Drawing.Image]::FromFile($InputPath)
$sizes = [int[]](16, 20, 24, 32, 40, 48, 64, 128, 256)
$frames = New-Object System.Collections.Generic.List[object]

try {
    foreach ($size in $sizes) {
        $bitmap = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $memory = New-Object System.IO.MemoryStream
        try {
            $graphics.Clear([System.Drawing.Color]::Transparent)
            $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $graphics.DrawImage($source, 0, 0, $size, $size)
            $bitmap.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
            $frames.Add([pscustomobject]@{
                Size = $size
                Data = $memory.ToArray()
            })
        }
        finally {
            $memory.Dispose()
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
}
finally {
    $source.Dispose()
}

$stream = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::Create)
$writer = New-Object System.IO.BinaryWriter $stream
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$frames.Count)

    [uint32]$offset = 6 + (16 * $frames.Count)
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.Data.Length)
        $writer.Write([uint32]$offset)
        $offset += [uint32]$frame.Data.Length
    }

    foreach ($frame in $frames) {
        $writer.Write([byte[]]$frame.Data)
    }
}
finally {
    $writer.Dispose()
    $stream.Dispose()
}

