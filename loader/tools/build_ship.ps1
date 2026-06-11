param(
    [string]$BuildDir = "",
    [string]$Config = "Release",
    [switch]$ServerPayload
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $BuildDir) {
    $BuildDir = Join-Path $repoRoot "build"
}

function Find-CMakeExecutable {
    $candidates = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $paths = & $vswhere -all -requires Microsoft.Component.MSBuild -property installationPath 2>$null
        foreach ($p in $paths) {
            if (-not $p) { continue }
            $c = Join-Path $p "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if ((Test-Path $c) -and ($candidates -notcontains $c)) { $candidates += $c }
        }
    }
    foreach ($c in @(
        "$env:ProgramFiles\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )) {
        if ((Test-Path $c) -and ($candidates -notcontains $c)) { $candidates += $c }
    }
    if ($candidates.Count -gt 0) { return $candidates[0] }
    return "cmake"
}

function Get-CMakeGeneratorArgs {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        return @("-G", "Visual Studio 17 2022", "-A", "x64")
    }

    $instances = @(& $vswhere -all -format json | ConvertFrom-Json)
    if ($instances.Count -eq 1 -and $null -eq $instances[0].installationPath) {
        $instances = @()
    }

    $vs18 = $instances | Where-Object { $_.installationVersion -match '^18\.' } | Select-Object -First 1
    if ($vs18 -and $vs18.isComplete -and $vs18.isLaunchable) {
        return @("-G", "Visual Studio 18 2026", "-A", "x64")
    }

    $vs17Path = & $vswhere -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null | Select-Object -First 1
    if ($vs17Path) {
        return @("-G", "Visual Studio 17 2022", "-A", "x64")
    }

    $buildToolsVc = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
    if (Test-Path $buildToolsVc) {
        return @("-G", "Visual Studio 17 2022", "-A", "x64")
    }

    if ($vs18) {
        Write-Warning "Visual Studio 2026 is installed but incomplete (open Visual Studio Installer and finish setup)."
        Write-Warning "Falling back to Visual Studio 17 2022 generator + Build Tools."
    }

    return @("-G", "Visual Studio 17 2022", "-A", "x64")
}

function Clear-CMakeCache {
    param([string]$BuildDir)
    $cacheFile = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $cacheFile) { Remove-Item $cacheFile -Force }
    $cmakeFilesDir = Join-Path $BuildDir "CMakeFiles"
    if (Test-Path $cmakeFilesDir) { Remove-Item $cmakeFilesDir -Recurse -Force }
}

function Test-CMakeCacheNeedsReset {
    param(
        [string]$BuildDir,
        [string[]]$GeneratorArgs
    )

    $cacheFile = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path $cacheFile)) { return $false }

    $desiredGen = $null
    for ($i = 0; $i -lt $GeneratorArgs.Length; $i++) {
        if ($GeneratorArgs[$i] -eq "-G" -and ($i + 1) -lt $GeneratorArgs.Length) {
            $desiredGen = $GeneratorArgs[$i + 1]
            break
        }
    }
    if (-not $desiredGen) { return $false }

    $head = Get-Content $cacheFile -TotalCount 8 -ErrorAction SilentlyContinue
    $header = ($head -join "`n")

    if ($desiredGen -match "17 2022" -and $header -match "Visual Studio\\18\\") {
        return $true
    }
    if ($desiredGen -match "18 2026" -and $header -notmatch "Visual Studio\\18\\") {
        return $true
    }
    return $false
}

$cmake = Find-CMakeExecutable
$generatorArgs = Get-CMakeGeneratorArgs

& (Join-Path $PSScriptRoot "write_build_version.ps1") -RepoRoot $repoRoot
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$configureArgs = @(
    "-B", $BuildDir,
    "-S", $repoRoot
) + $generatorArgs + @(
    "-DBUILD_CRYMORE_LOADER=ON",
    "-DBUILD_LOADER_WPF=ON",
    "-DBUILD_LOADER_NATIVE_UI=OFF"
)
if ($ServerPayload) {
    $configureArgs += "-DCRYMORE_SERVER_PAYLOAD=ON"
}

if (Test-CMakeCacheNeedsReset -BuildDir $BuildDir -GeneratorArgs $generatorArgs) {
    Write-Host "==> Clearing stale CMake cache (generator mismatch)"
    Clear-CMakeCache -BuildDir $BuildDir
}

Write-Host "==> Configure"
Write-Host "    Source: $repoRoot"
Write-Host "    Build:  $BuildDir"
Write-Host "    CMake:  $cmake"
Write-Host "    Generator: $($generatorArgs -join ' ')"
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    if (Test-Path (Join-Path $BuildDir "CMakeCache.txt")) {
        Write-Host "==> Configure failed; clearing cache and retrying once"
        Clear-CMakeCache -BuildDir $BuildDir
        & $cmake @configureArgs
    }
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "==> Build kernel driver (WDK)"
& (Join-Path $PSScriptRoot "build_kernel_driver.ps1") -RepoRoot $repoRoot -Config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Build overlay + loader ($Config)"
& $cmake --build $BuildDir --config $Config --target cs2_overlay crymore_core crymore_loader_wpf
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$overlayOut = Join-Path $BuildDir $Config
Write-Host "==> Build phys_extract.exe (no map tris - R2 300 MiB limit)"
& (Join-Path $PSScriptRoot "build_phys_extract.ps1") -OutDir $overlayOut
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> Copy CS2 icon fonts into overlay output"
& (Join-Path $PSScriptRoot "copy_csgo_fonts.ps1") -ReleaseDir $overlayOut

$out = Join-Path $BuildDir "loader\$Config"
Write-Host ""
Write-Host "Ship folder: $out"
Write-Host "  Crymore.Loader.exe"
Write-Host "  crymore_core.dll"
if (-not $ServerPayload) {
    Write-Host "  (embedded encrypted overlay in crymore_core.dll)"
} else {
    Write-Host "  (slim build - overlay fetched from server at launch)"
}
