# SPDX-License-Identifier: GPL-3.0-or-later
param(
  [Parameter(Mandatory = $true)]
  [string]$Source,
  [string]$Output = "",
  [string]$Object = ""
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

if ($null -eq (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
  throw "clang-cl was not found in PATH."
}

if ($null -eq (Get-Command link.exe -ErrorAction SilentlyContinue)) {
  throw "link.exe was not found in PATH. Run 'msvc' in this PowerShell before linking CRT examples."
}

$sourcePath = Resolve-RepoPath $Source
$baseName = [System.IO.Path]::GetFileNameWithoutExtension($sourcePath)

if ($Object.Length -eq 0) {
  $objectPath = Join-Path $repoRoot "build\examples\$baseName.obj"
} elseif ([System.IO.Path]::IsPathRooted($Object)) {
  $objectPath = $Object
} else {
  $objectPath = Join-Path $repoRoot $Object
}

if ($Output.Length -eq 0) {
  $outputPath = Join-Path $repoRoot "build\examples\$baseName.exe"
} elseif ([System.IO.Path]::IsPathRooted($Output)) {
  $outputPath = $Output
} else {
  $outputPath = Join-Path $repoRoot $Output
}

New-Item -ItemType Directory -Force (Split-Path -Parent $objectPath) | Out-Null
New-Item -ItemType Directory -Force (Split-Path -Parent $outputPath) | Out-Null

Push-Location $repoRoot
try {
  & $compiler --emit-obj -I include --sysroot sysroot --output $objectPath $sourcePath
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }

  $linkArguments = @(
    "@$repoRoot\scripts\clang_cl_common.rsp",
    "@$repoRoot\scripts\clang_cl_debug.rsp",
    $objectPath,
    "/Fe$outputPath",
    "/link",
    "/nologo",
    "/subsystem:console",
    "/defaultlib:libcmt",
    "/defaultlib:libucrt",
    "/defaultlib:libvcruntime",
    "/defaultlib:oldnames",
    "/defaultlib:legacy_stdio_definitions",
    "/defaultlib:kernel32"
  )

  & clang-cl @linkArguments
  if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
  }
} finally {
  Pop-Location
}

Write-Host "Built $outputPath"
