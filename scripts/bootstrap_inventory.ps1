# SPDX-License-Identifier: GPL-3.0-or-later
param(
  [string]$Compiler = "$PSScriptRoot\..\build\bin\fcc.exe",
  [string]$Sysroot = "$PSScriptRoot\..\sysroot",
  [switch]$FailOnError
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path "$PSScriptRoot\.."
$compilerPath = Resolve-Path $Compiler -ErrorAction SilentlyContinue
if ($null -eq $compilerPath) {
  Write-Error "FCC compiler was not found at '$Compiler'. Run 'make debug' first."
}
$sysrootPath = Resolve-Path $Sysroot -ErrorAction SilentlyContinue
if ($null -eq $sysrootPath) {
  Write-Error "FCC sysroot was not found at '$Sysroot'."
}

$sources = Get-ChildItem -Path "$repoRoot\src" -Filter "*.c" | Sort-Object Name
$failures = 0

Push-Location $repoRoot
try {
  foreach ($source in $sources) {
    $relativePath = "src\$($source.Name)"
    Write-Host "Checking $relativePath"
    & $compilerPath --check -I include --sysroot $sysrootPath $relativePath
    if ($LASTEXITCODE -eq 0) {
      Write-Host "  OK"
    } else {
      Write-Host "  FAIL exit=$LASTEXITCODE"
      $failures += 1
    }
  }
} finally {
  Pop-Location
}

Write-Host "Self-hosting inventory: $($sources.Count - $failures) passed, $failures failed."

if ($FailOnError -and ($failures -ne 0)) {
  exit 1
}

exit 0
