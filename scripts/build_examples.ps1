# SPDX-License-Identifier: GPL-3.0-or-later
param(
  [switch]$RunFinite
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$internalScript = Join-Path $PSScriptRoot "build_fcc_internal_example.ps1"
$crtScript = Join-Path $PSScriptRoot "build_fcc_crt_example.ps1"

& $internalScript -Source "examples\minimal_return.c"
& $internalScript -Source "examples\infinite_loop.c"

& $crtScript -Source "examples\hello_world_libc.c"
& $crtScript -Source "examples\libc_counter.c"
& $crtScript -Source "examples\infinite_hello_libc.c"

if ($RunFinite) {
  $hello = Join-Path $repoRoot "build\examples\hello_world_libc.exe"
  $counter = Join-Path $repoRoot "build\examples\libc_counter.exe"

  & $hello
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }

  & $counter
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
}
