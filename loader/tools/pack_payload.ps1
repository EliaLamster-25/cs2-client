param(
    [Parameter(Mandatory = $true)][string]$ReleaseDir,
    [Parameter(Mandatory = $true)][string]$OutBin,
    [string]$IdentityHeader = ""
)

$ErrorActionPreference = 'Stop'

if (-not $IdentityHeader) {
    $IdentityHeader = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "generated\driver_identity.h"
}

function Get-DriverIdentity {
    param([string]$HeaderPath)
    $overlayExeName = $null
    $mapperExeName = $null
    $driverSysName = $null
    if (-not (Test-Path $HeaderPath)) {
        Write-Warning "driver_identity.h not found at $HeaderPath - falling back to newest root .exe"
        return @{ HostExe = $null; MapperExe = $null; SysFile = $null }
    }
    foreach ($line in Get-Content $HeaderPath) {
        if ($line -like '*DRV_HOST_EXE_NAME*') {
            $parts = $line -split '"'
            if ($parts.Length -ge 2) { $overlayExeName = $parts[1] }
        }
        if ($line -like '*DRV_MAPPER_EXE_NAME*') {
            $parts = $line -split '"'
            if ($parts.Length -ge 2) { $mapperExeName = $parts[1] }
        }
        if ($line -like '*DRV_SYS_FILE_NAME*') {
            $parts = $line -split '"'
            if ($parts.Length -ge 2) { $driverSysName = $parts[1] }
        }
    }
    return @{ HostExe = $overlayExeName; MapperExe = $mapperExeName; SysFile = $driverSysName }
}

$identity = Get-DriverIdentity -HeaderPath $IdentityHeader
$meshWhitelist = @(
    'meshes/tm_phoenix.glb',
    'meshes/ctm_sas.glb'
)

function Should-PackFile {
    param([string]$Rel, [hashtable]$Id)
    $r = $Rel.Replace('\', '/').ToLowerInvariant()

    if ($r -match '\.(pdb|ilk|exp|lib|tlog|log|recipe|ini)$') { return $false }
    if ($r -match 'crymore_loader\.exe$') { return $false }
    if ($r -match '(^|/)configs/') { return $false }
    if ($r -match 'kdmapper_(err|out)\.txt$') { return $false }

    if ($r -match '^assets/') { return $true }

    if ($r -match '^mapcache/') { return $true }

    # phys_tools is build-time only; collision tris ship in mapcache/

    if ($r -match '^meshes/') {
        return $script:meshWhitelist -contains $r
    }

    if ($r -notmatch '/') {
        $base = $Rel.Replace('\', '/')
        if ($Id.HostExe -and $base -ieq $Id.HostExe) { return $true }
        if ($Id.MapperExe -and $base -ieq $Id.MapperExe) { return $true }
        if ($Id.SysFile -and $base -ieq $Id.SysFile) { return $true }
        if ($base -ieq 'steam_api64.dll') { return $true }
        if ($base -ieq 'steam_appid.txt') { return $true }
        if ($base -ieq 'csgo_icons.ttf') { return $true }
        if ($base -ieq 'csgo_icons_outline.ttf') { return $true }
        if ($base -imatch '^t_bot\.jpeg$') { return $true }
        if ($base -imatch '^ct_bot\.jpeg$') { return $true }
        return $false
    }

    return $false
}

$files = @()
if (Test-Path $ReleaseDir) {
    $files = Get-ChildItem -Path $ReleaseDir -Recurse -File |
        Where-Object {
            $rel = $_.FullName.Substring($ReleaseDir.Length).TrimStart('\', '/')
            (Should-PackFile $rel $identity) -and $_.Length -ge 0
        } |
        Sort-Object FullName
}

# Fallback: if identity files missing from Release, pick newest root overlay exe + matching sys/mapper by mtime
if ($identity.HostExe -and -not ($files | Where-Object { $_.Name -ieq $identity.HostExe })) {
    Write-Error "Host exe $($identity.HostExe) not in ReleaseDir - rebuild cs2_overlay first."
}
if ($identity.SysFile -and -not ($files | Where-Object { $_.Name -ieq $identity.SysFile })) {
    Write-Error "Driver $($identity.SysFile) not in ReleaseDir - rebuild PooDriver kernel driver, then cs2_overlay."
}
if ($identity.MapperExe -and -not ($files | Where-Object { $_.Name -ieq $identity.MapperExe })) {
    Write-Error "Mapper $($identity.MapperExe) not in ReleaseDir - rebuild kdmapper, then cs2_overlay."
}
if ($files.Count -eq 0) {
    $newest = Get-ChildItem $ReleaseDir -File -Filter *.exe |
        Where-Object { $_.Name -notmatch 'crymore|loader' } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($newest) {
        Write-Warning "Packing fallback newest exe only: $($newest.Name)"
        $files = @($newest)
    }
}

$ms = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($ms)
$writer.Write([UInt32]0x474D5243) # CRMG
$writer.Write([UInt32]1)
$writer.Write([UInt32]$files.Count)

$totalBytes = 0L
foreach ($f in $files) {
    $rel = $f.FullName.Substring($ReleaseDir.Length).TrimStart('\', '/').Replace("\", "/")
    $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($rel)
    $data = if ($f.Length -eq 0) { [byte[]]@() } else { [IO.File]::ReadAllBytes($f.FullName) }
    $writer.Write([UInt32]$nameBytes.Length)
    $writer.Write($nameBytes, 0, $nameBytes.Length)
    $writer.Write([UInt32]$data.Length)
    if ($data.Length -gt 0) { $writer.Write($data, 0, $data.Length) }
    $totalBytes += $data.Length
}

$writer.Flush()
$bytes = $ms.ToArray()

$outDir = Split-Path -Parent $OutBin
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
[IO.File]::WriteAllBytes($OutBin, $bytes)

$mb = [math]::Round($totalBytes / 1MB, 2)
Write-Host "Wrote payload.bin ($($bytes.Length) bytes archive, $mb MB payload, $($files.Count) files)"
if ($identity.HostExe) {
    Write-Host "  host: $($identity.HostExe)"
    Write-Host "  mapper: $($identity.MapperExe)"
    Write-Host "  driver: $($identity.SysFile)"
}
