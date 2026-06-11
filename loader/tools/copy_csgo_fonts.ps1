param(
    [Parameter(Mandatory = $true)][string]$ReleaseDir
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

function Get-SteamRoot {
    $paths = @(
        @{ Root = 'HKCU:\Software\Valve\Steam'; Name = 'SteamPath' },
        @{ Root = 'HKLM:\Software\WOW6432Node\Valve\Steam'; Name = 'InstallPath' }
    )
    foreach ($p in $paths) {
        try {
            $val = (Get-ItemProperty -Path $p.Root -Name $p.Name -ErrorAction Stop).($p.Name)
            if ($val) { return $val.TrimEnd('\', '/') }
        } catch {}
    }
    return $null
}

if (-not (Test-Path $ReleaseDir)) {
    New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null
}

$steam = Get-SteamRoot
$cs2Fonts = $null
if ($steam) {
    $candidate = Join-Path $steam 'steamapps\common\Counter-Strike Global Offensive\game\csgo\panorama\fonts'
    if (Test-Path $candidate) { $cs2Fonts = $candidate }
}

foreach ($name in @('csgo_icons.ttf', 'csgo_icons_outline.ttf')) {
    $dst = Join-Path $ReleaseDir $name
    $repoSrc = Join-Path $repoRoot $name
    if (Test-Path $repoSrc) {
        Copy-Item -Path $repoSrc -Destination $dst -Force
        Write-Host "Copied $name from repo -> $dst"
        continue
    }

    if (-not $cs2Fonts) { continue }
    $src = Join-Path $cs2Fonts $name
    if (-not (Test-Path $src)) {
        Write-Warning "Missing $name (not in repo or CS2 install)."
        continue
    }
    Copy-Item -Path $src -Destination $dst -Force
    Write-Host "Copied $name from CS2 -> $dst"
}
