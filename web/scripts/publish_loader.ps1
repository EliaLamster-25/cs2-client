param(
    [string]$LoaderDir = "",
    [string]$OverlayDir = "",
    [string]$OutZip = "$PSScriptRoot\..\dist\crymore-loader.zip",
    [string]$Version = "",
    [string]$ApiBase = "https://crymore.crymore-pw.workers.dev",
    [string]$AdminSecret = "",
    [switch]$Build,
    [switch]$BumpPatch,
    [switch]$NoBump
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

. (Join-Path $PSScriptRoot "load_publish_env.ps1") -RepoRoot $repoRoot

if (-not $AdminSecret) {
    if ($env:CRYMORE_ADMIN_SECRET) { $AdminSecret = $env:CRYMORE_ADMIN_SECRET }
    elseif ($env:ADMIN_SECRET) { $AdminSecret = $env:ADMIN_SECRET }
}

$versionFile = Join-Path $repoRoot "web\VERSION"
if (-not $Version -and (Test-Path $versionFile)) {
    $Version = (Get-Content $versionFile -Raw).Trim()
}
if (-not $Version) { $Version = "1.1.4" }

$shouldBumpPatch = $BumpPatch -or ($Build -and -not $NoBump -and -not $PSBoundParameters.ContainsKey("Version"))
if ($shouldBumpPatch -and $Version -match '^(\d+)\.(\d+)\.(\d+)$') {
    $Version = "{0}.{1}.{2}" -f [int]$Matches[1], [int]$Matches[2], ([int]$Matches[3] + 1)
    Set-Content -Path $versionFile -Value $Version -NoNewline
    Write-Host "Bumped release version -> v$Version"
}

if (-not $LoaderDir) { $LoaderDir = Join-Path $repoRoot "build\loader\Release" }
if (-not $OverlayDir) { $OverlayDir = Join-Path $repoRoot "build\Release" }

$keyFile = Join-Path $repoRoot "loader\.payload_key"
if (-not $env:PAYLOAD_KEY -and -not (Test-Path $keyFile)) {
    Write-Error @"
Missing PAYLOAD_KEY.
  1. Copy loader/.payload_key.example to loader/.payload_key
  2. Set the same value in Cloudflare: npx wrangler secret put PAYLOAD_KEY
"@
}

if (-not $AdminSecret) {
    Write-Error @"
Missing admin secret for POST /v1/admin/releases.
  Set CRYMORE_ADMIN_SECRET, or add ADMIN_SECRET=... to web/.dev.vars (same value as wrangler secret ADMIN_SECRET).
"@
}

Write-Host "==> Publish v$Version to $ApiBase"

if ($Build) {
    Write-Host "==> Building Release overlay + slim server-payload loader..."
    & (Join-Path $repoRoot "loader\tools\write_build_version.ps1") -RepoRoot $repoRoot -Version $Version
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $repoRoot "loader\tools\build_ship.ps1") -ServerPayload
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path $OverlayDir)) {
    Write-Error "Overlay Release folder not found: $OverlayDir (run with -Build or build cs2_overlay first)."
}

$genDir = Join-Path $repoRoot "build\loader\generated"
if (-not (Test-Path $genDir)) {
    New-Item -ItemType Directory -Path $genDir -Force | Out-Null
}

$plainBin = Join-Path $genDir "overlay_plain.bin"
$overlayEnc = Join-Path $genDir "overlay.enc"
$packScript = Join-Path $repoRoot "loader\tools\pack_payload.ps1"
$encryptTool = Join-Path $repoRoot "loader\tools\PayloadEncrypt"

Write-Host "==> Packing overlay from $OverlayDir"
$identityHeader = Join-Path $repoRoot "generated\driver_identity.h"
& $packScript -ReleaseDir $OverlayDir -OutBin $plainBin -IdentityHeader $identityHeader
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$dekBytes = [byte[]]::new(32)
$rng = New-Object System.Security.Cryptography.RNGCryptoServiceProvider
$rng.GetBytes($dekBytes)
$dekB64 = [Convert]::ToBase64String($dekBytes)

Write-Host "==> Encrypting overlay with release DEK"
dotnet build $encryptTool -c Release --nologo -v q
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
dotnet run --project $encryptTool -c Release --no-build -- $plainBin $overlayEnc $dekB64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$overlayR2Key = "releases/stable/$Version/overlay.enc"
Write-Host "==> Uploading overlay to R2: $overlayR2Key ($((Get-Item $overlayEnc).Length) bytes)"
Push-Location (Join-Path $PSScriptRoot "..")
try {
    npx wrangler r2 object put "crymore-loaders/$overlayR2Key" --file $overlayEnc
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    Pop-Location
}

$packLoaderScript = Join-Path $PSScriptRoot "pack_loader.ps1"
Write-Host "==> Packing loader zip"
& $packLoaderScript -LoaderDir $LoaderDir -Version $Version
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$zipPath = Join-Path (Split-Path $packLoaderScript -Parent) "..\dist\crymore-loader.zip"
if (-not (Test-Path $zipPath)) {
    Write-Error "Pack failed - crymore-loader.zip was not created."
}
$zip = (Resolve-Path $zipPath).Path

$r2Key = "releases/stable/$Version/crymore-loader.zip"
Write-Host "==> Uploading loader zip to R2: $r2Key ($((Get-Item $zip).Length) bytes)"
Push-Location (Join-Path $PSScriptRoot "..")
try {
    npx wrangler r2 object put "crymore-loaders/$r2Key" --file $zip
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    Pop-Location
}

$body = @{
    version          = $Version
    channel          = "stable"
    r2_key           = $r2Key
    file_name        = "crymore-loader.zip"
    overlay_r2_key   = $overlayR2Key
    overlay_dek_b64  = $dekB64
} | ConvertTo-Json

Write-Host "==> Registering release v$Version (loader + server overlay)"
$result = $null
try {
    $result = Invoke-RestMethod -Method POST -Uri "$ApiBase/v1/admin/releases" `
        -Headers @{ "x-admin-secret" = $AdminSecret; "content-type" = "application/json" } `
        -Body $body
}
catch {
    Write-Warning "Admin API registration failed; falling back to direct Wrangler D1 insert."
    function SqlQuote([string]$s) {
        if ($null -eq $s) { return "NULL" }
        return "'" + $s.Replace("'", "''") + "'"
    }

    $releaseId = [Guid]::NewGuid().ToString("N")
    $publishedAt = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $sql = "INSERT INTO releases (id, version, channel, r2_key, file_name, overlay_r2_key, overlay_dek_b64, published_at) VALUES ($(SqlQuote $releaseId), $(SqlQuote $Version), 'stable', $(SqlQuote $r2Key), 'crymore-loader.zip', $(SqlQuote $overlayR2Key), $(SqlQuote $dekB64), $publishedAt);"

    Push-Location (Join-Path $PSScriptRoot "..")
    try {
        npx wrangler d1 execute crymore --remote --command "$sql"
        if ($LASTEXITCODE -ne 0) { throw "Wrangler D1 release insert failed." }
        $result = @{ ok = $true; id = $releaseId; version = $Version; channel = "stable"; fallback = "d1" }
    }
    finally {
        Pop-Location
    }
}

Write-Host ""
Write-Host "Done. v$Version is live on the download page."
Write-Host "  Dashboard: $ApiBase/dashboard.html"
Write-Host "  Download shows: v$Version"
if ($result) {
    Write-Host "  API: $($result | ConvertTo-Json -Compress)"
}
