param(
    [Parameter(Mandatory = $true)][string]$OutBin
)

$ErrorActionPreference = 'Stop'

$ms = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter($ms)
$writer.Write([UInt32]0x474D5243) # CRMG
$writer.Write([UInt32]1)
$writer.Write([UInt32]0)
$writer.Flush()

$outDir = Split-Path -Parent $OutBin
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}
[IO.File]::WriteAllBytes($OutBin, $ms.ToArray())
Write-Host "Wrote empty payload stub ($($ms.Length) bytes)"
