param(
    [string]$RepoRoot = ""
)

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
}

function Import-DotEnvFile {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return }
    Get-Content $Path | ForEach-Object {
        $line = $_.Trim()
        if (-not $line -or $line.StartsWith('#')) { return }
        $idx = $line.IndexOf('=')
        if ($idx -lt 1) { return }
        $name = $line.Substring(0, $idx).Trim()
        $value = $line.Substring($idx + 1).Trim()
        if (($value.StartsWith('"') -and $value.EndsWith('"')) -or ($value.StartsWith("'") -and $value.EndsWith("'"))) {
            $value = $value.Substring(1, $value.Length - 2)
        }
        if (-not [string]::IsNullOrWhiteSpace($name)) {
            Set-Item -Path "Env:$name" -Value $value
        }
    }
}

Import-DotEnvFile (Join-Path $RepoRoot "web\.dev.vars")

$keyFile = Join-Path $RepoRoot "loader\.payload_key"
if (-not $env:PAYLOAD_KEY -and (Test-Path $keyFile)) {
    $env:PAYLOAD_KEY = (Get-Content $keyFile -Raw).Trim()
}

if (-not $env:CRYMORE_ADMIN_SECRET -and $env:ADMIN_SECRET) {
    $env:CRYMORE_ADMIN_SECRET = $env:ADMIN_SECRET
}
