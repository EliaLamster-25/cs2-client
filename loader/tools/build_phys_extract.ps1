param(
    [string]$OutDir = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "build\Release"),
    [switch]$IncludeMapTris
)

$ErrorActionPreference = 'Stop'

$projDir = Join-Path $PSScriptRoot "..\..\cphys-extractor_[unknowncheats.me]_"
$projDir = (Resolve-Path -LiteralPath $projDir).Path

$toolDir = Join-Path $OutDir "phys_tools"
if (Test-Path -LiteralPath $toolDir) {
    Remove-Item -LiteralPath $toolDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $toolDir | Out-Null

Write-Host "Publishing PhysExtractor -> phys_tools/ (multi-file, avoids single-file .NET crash from temp)"
Push-Location -LiteralPath $projDir
try {
    dotnet publish -c Release -r win-x64 --self-contained true `
        /p:PublishSingleFile=false `
        -o $toolDir
} finally {
    Pop-Location
}

$published = Join-Path $toolDir "PhysExtractor.exe"
if (-not (Test-Path -LiteralPath $published)) {
    Write-Error "Publish failed - PhysExtractor.exe not found in phys_tools"
}

# Legacy name used by overlay launcher
Copy-Item -LiteralPath $published -Destination (Join-Path $toolDir "phys_extract.exe") -Force

# Remove old single-file copy at Release root if present
$legacy = Join-Path $OutDir "phys_extract.exe"
if (Test-Path -LiteralPath $legacy) {
    Remove-Item -LiteralPath $legacy -Force
}

$fileCount = (Get-ChildItem -LiteralPath $toolDir -File).Count
$sizeMb = [math]::Round((Get-ChildItem -LiteralPath $toolDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "Wrote phys_tools ($fileCount files, $sizeMb MB)"

# Pre-extract collision for active map pool (bundled in payload - avoids runtime .NET spawn).
$priorityMaps = @(
    'de_dust2', 'de_mirage', 'de_inferno', 'de_nuke', 'de_ancient', 'de_anubis', 'de_vertigo'
)
$mapCache = Join-Path $OutDir "mapcache"
New-Item -ItemType Directory -Force -Path $mapCache | Out-Null
$extracted = 0
$cacheMb = 0.0
foreach ($map in $priorityMaps) {
    $outTri = Join-Path $mapCache ($map + ".tri")
    if (Test-Path -LiteralPath $outTri) {
        $extracted++
        $cacheMb += (Get-Item -LiteralPath $outTri).Length / 1MB
        continue
    }
    Write-Host "  Extracting $map ..."
    & $published --map $map --tri-dir $mapCache 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $outTri)) {
        $extracted++
        $cacheMb += (Get-Item -LiteralPath $outTri).Length / 1MB
    } else {
        Write-Host "  (skipped $map - CS2 VPK missing or extract failed)"
    }
}
Write-Host "Bundled mapcache: $extracted maps ($([math]::Round($cacheMb, 1)) MB)"

# phys_tools only needed for build-time extraction — omit from ship payload
Remove-Item -LiteralPath $toolDir -Recurse -Force
Write-Host "Removed phys_tools from Release (map tris only in mapcache/)"

if ($IncludeMapTris) {
    $mapCache = Join-Path $OutDir "mapcache"
    New-Item -ItemType Directory -Force -Path $mapCache | Out-Null
    $triSources = @(
        (Join-Path $projDir "bin\Release\net9.0\tri"),
        (Join-Path $projDir "bin\Release\net9.0\win-x64\tri")
    )
    $copied = 0
    foreach ($triDir in $triSources) {
        if (-not (Test-Path -LiteralPath $triDir)) { continue }
        Get-ChildItem -LiteralPath $triDir -Filter "*.tri" | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $mapCache $_.Name) -Force
            $copied++
        }
    }
    Write-Host "Local mapcache: copied $copied tri files (NOT for server publish)"
} elseif (-not $extracted) {
    if (Test-Path -LiteralPath $mapCache) {
        Remove-Item -LiteralPath $mapCache -Recurse -Force
        Write-Host "Removed empty Release\mapcache"
    }
}
