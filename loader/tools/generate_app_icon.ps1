param(
    [string]$PngPath = "$PSScriptRoot\..\resources\brand_logo.png",
    [string]$OutIco = "$PSScriptRoot\..\csharp\Crymore.Loader\Assets\app.ico"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $PngPath)) {
    Write-Error "Logo not found: $PngPath"
}

$outDir = Split-Path $OutIco -Parent
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

$src = [System.Drawing.Image]::FromFile((Resolve-Path $PngPath))
try {
    $bmp = New-Object System.Drawing.Bitmap($src, 256, 256)
    $hIcon = $bmp.GetHicon()
    $icon = [System.Drawing.Icon]::FromHandle($hIcon)
    $fs = [System.IO.File]::Open($OutIco, [System.IO.FileMode]::Create)
    try {
        $icon.Save($fs)
    } finally {
        $fs.Close()
    }
    Write-Host "Wrote $OutIco"
} finally {
    $src.Dispose()
}
