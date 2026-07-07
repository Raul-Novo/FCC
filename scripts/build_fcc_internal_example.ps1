# SPDX-License-Identifier: GPL-3.0-or-later
param(
  [Parameter(Mandatory = $true)]
  [string]$Source,
  [string]$Output = ""
)

$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
  param([string]$Path)

  if ([System.IO.Path]::IsPathRooted($Path)) {
    return (Resolve-Path -LiteralPath $Path).Path
  }

  return (Resolve-Path -LiteralPath (Join-Path $repoRoot $Path)).Path
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$compiler = Join-Path $repoRoot "build\bin\fcc.exe"
if (!(Test-Path -LiteralPath $compiler)) {
  throw "FCC compiler not found at '$compiler'. Run: msvc; make debug"
}

$sourcePath = Resolve-RepoPath $Source
if ($Output.Length -eq 0) {
  $baseName = [System.IO.Path]::GetFileNameWithoutExtension($sourcePath)
  $outputPath = Join-Path $repoRoot "build\examples\$baseName.exe"
} elseif ([System.IO.Path]::IsPathRooted($Output)) {
  $outputPath = $Output
} else {
  $outputPath = Join-Path $repoRoot $Output
}

$outputDirectory = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Force $outputDirectory | Out-Null

Push-Location $repoRoot
try {
  & $compiler --compile-and-link --output $outputPath $sourcePath
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
} finally {
  Pop-Location
}

Write-Host "Built $outputPath"
