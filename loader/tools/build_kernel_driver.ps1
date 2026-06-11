param(
    [string]$RepoRoot = "",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

$identityHeader = Join-Path $RepoRoot "generated\driver_identity.h"
if (-not (Test-Path $identityHeader)) {
    Write-Error "Missing $identityHeader - run cmake configure first."
}

Copy-Item -Force $identityHeader (Join-Path $RepoRoot "PooDriver\driver_identity.h")

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "Visual Studio vswhere not found. Install VS with C++ and WDK."
}

$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) {
    Write-Error "MSBuild not found."
}

$kitsRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
$buildRoot = Join-Path $kitsRoot "build"
if (-not (Test-Path $buildRoot)) {
    Write-Error @"
Windows SDK/WDK build folder not found at $buildRoot.
Install the WDK: https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk
"@
}

$platformVersion = $null
foreach ($dir in Get-ChildItem $buildRoot -Directory | Sort-Object Name -Descending) {
    $targets = Join-Path $dir.FullName "WindowsDriver.Common.targets"
    if (Test-Path $targets) {
        $platformVersion = $dir.Name
        break
    }
}

if (-not $platformVersion) {
    Write-Error @"
WindowsDriver.Common.targets was not found under $buildRoot.
You likely have the Windows SDK but not the WDK driver build targets.
Install the WDK (matching your VS version), then retry.
"@
}

$kmHeader = Join-Path $kitsRoot "Include\$platformVersion\km\ntddk.h"
if (-not (Test-Path $kmHeader)) {
    Write-Error "Kernel-mode WDK headers missing at $kmHeader. Reinstall the WDK."
}

$proj = Join-Path $RepoRoot "PooDriver\MemReaderKdmp.vcxproj"
Write-Host "==> Building kernel driver ($Config|x64)"
Write-Host "    Identity: $identityHeader"
Write-Host "    WDK platform: $platformVersion"

& $msbuild $proj /p:Configuration=$Config /p:Platform=x64 /p:WindowsTargetPlatformVersion=$platformVersion /p:SignMode=Off /p:ApiValidator_Enable=false /v:minimal
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sys = Join-Path $RepoRoot "PooDriver\x64\$Config\MemReaderKdmp.sys"
if (-not (Test-Path $sys)) {
    Write-Error "Driver build finished but MemReaderKdmp.sys was not produced at $sys"
}

Write-Host "    Built: $sys ($((Get-Item $sys).Length) bytes)"
