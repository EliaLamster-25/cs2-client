param(
    [string]$LoaderDir = "",
    [string]$OutZip = "$PSScriptRoot\..\dist\crymore-loader.zip",
    [string]$Version = "",
    [switch]$SelfContained
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $LoaderDir) { $LoaderDir = Join-Path $repoRoot "build\loader\Release" }
if (-not $Version) {
    $versionFile = Join-Path $repoRoot "web\VERSION"
    if (Test-Path $versionFile) {
        $Version = (Get-Content $versionFile -Raw).Trim()
    } else {
        $Version = "0.0.0-dev"
    }
}
& (Join-Path $repoRoot "loader\tools\write_build_version.ps1") -RepoRoot $repoRoot -Version $Version
$csproj = Join-Path $repoRoot "loader\csharp\Crymore.Loader\Crymore.Loader.csproj"
$coreDll = Join-Path $LoaderDir "crymore_core.dll"

if (-not (Test-Path $coreDll)) {
    Write-Error "Missing $coreDll - run loader\tools\build_ship.ps1 first"
}

$dist = Split-Path $OutZip -Parent
New-Item -ItemType Directory -Force -Path $dist | Out-Null
if (Test-Path $OutZip) { Remove-Item $OutZip -Force }

$staging = Join-Path $env:TEMP "crymore-loader-pack-$(Get-Random)"
New-Item -ItemType Directory -Force -Path $staging | Out-Null

# Zip ships the single-file exe, native core DLL, and version metadata.
$keep = @("Crymore.Loader.exe", "crymore_core.dll", "version.txt")

try {
    $publishArgs = @(
        "publish", $csproj,
        "-c", "Release",
        "-r", "win-x64",
        "-p:PublishSingleFile=true",
        "-p:IncludeNativeLibrariesForSelfExtract=true",
        "-p:SatelliteResourceLanguages=en",
        "-p:DebugType=none",
        "-p:DebugSymbols=false",
        "-p:Version=$Version",
        "-p:AssemblyVersion=$Version",
        "-p:FileVersion=$Version",
        "-p:CoreOutputDir=$LoaderDir",
        "-o", $staging
    )

    if ($SelfContained) {
        Write-Host "Publishing self-contained single-file loader (win-x64)..."
        $publishArgs += @(
            "--self-contained", "true",
            "-p:EnableCompressionInSingleFile=true"
        )
    }
    else {
        Write-Host "Publishing single-file loader (requires .NET 8 Desktop Runtime)..."
        $publishArgs += @(
            "--self-contained", "false",
            "-p:EnableCompressionInSingleFile=false"
        )
    }

    & dotnet @publishArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Copy-Item $coreDll (Join-Path $staging "crymore_core.dll") -Force
    Copy-Item (Join-Path $repoRoot "generated\version.txt") (Join-Path $staging "version.txt") -Force
    Get-ChildItem $staging -Filter "*.pdb" -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue

    Get-ChildItem $staging -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($staging.Length).TrimStart('\', '/')
        if ($rel -match '[\\/]') {
            Remove-Item $_.FullName -Force
            return
        }
        if ($keep -notcontains $_.Name) {
            Write-Host "  Dropping extra: $($_.Name)"
            Remove-Item $_.FullName -Force
        }
    }

    Get-ChildItem $staging -Directory -Recurse | Sort-Object FullName -Descending | ForEach-Object {
        Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
    }

    $packed = Get-ChildItem $staging -File | Sort-Object Name
    if ($packed.Count -eq 0) {
        Write-Error "Nothing to pack - publish produced no files."
    }
    if ($packed.Count -ne 3) {
        Write-Error "Expected exactly 3 files (Crymore.Loader.exe + crymore_core.dll + version.txt), got $($packed.Count)."
    }

    Write-Host ("Packing 3 files for v{0}:" -f $Version)
    $packed | ForEach-Object { Write-Host ('  {0} ({1} bytes)' -f $_.Name, $_.Length) }

    Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $OutZip -Force

    $size = (Get-Item $OutZip).Length
    Write-Host ('Created {0} ({1} bytes)' -f $OutZip, $size)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $OutZip).Path)
    try {
        Write-Host "Zip contains $($z.Entries.Count) entries"
        $z.Entries | ForEach-Object { Write-Host "  $($_.FullName)" }
    }
    finally {
        $z.Dispose()
    }
}
finally {
    if (Test-Path $staging) { Remove-Item $staging -Recurse -Force -ErrorAction SilentlyContinue }
}
