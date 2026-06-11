param(
    [Parameter(Mandatory = $true)][string]$InBin,
    [Parameter(Mandatory = $true)][string]$OutBin
)

$ErrorActionPreference = 'Stop'

$keyB64 = $env:PAYLOAD_KEY
$keyFile = Join-Path $PSScriptRoot "..\.payload_key"
if (-not $keyB64 -and (Test-Path $keyFile)) {
    $keyB64 = (Get-Content $keyFile -Raw).Trim()
}

if (-not $keyB64) {
    Write-Host "PAYLOAD_KEY not set - embedding plaintext payload (dev only)."
    Copy-Item $InBin $OutBin -Force
    exit 0
}

$toolDir = Join-Path $PSScriptRoot "PayloadEncrypt"
dotnet build $toolDir -c Release --nologo -v q
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
dotnet run --project $toolDir -c Release --no-build -- $InBin $OutBin $keyB64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
