# SPDX-License-Identifier: GPL-3.0-or-later
param(
  [string]$Compiler = "$PSScriptRoot\..\build\bin\fcc.exe",
  [string]$Sysroot = "$PSScriptRoot\..\sysroot",
  [string]$BuildRoot = "$PSScriptRoot\..\build\selfhost"
)

$ErrorActionPreference = "Stop"

function Invoke-Checked {
  param(
    [string]$Label,
    [string]$Program,
    [string[]]$Arguments
  )

  Write-Host $Label
  & $Program @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$Label failed with exit code $LASTEXITCODE"
  }
}

function Emit-CompilerAssembly {
  param(
    [string]$CompilerPath,
    [System.IO.FileInfo[]]$Sources,
    [string]$OutputDirectory,
    [string]$SysrootPath
  )

  New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
  foreach ($source in $Sources) {
    $outputPath = Join-Path $OutputDirectory ($source.BaseName + ".asm")
    Invoke-Checked "emit $($source.Name)" $CompilerPath @(
      "--emit-asm",
      "-I",
      "include",
      "--sysroot",
      $SysrootPath,
      "--output",
      $outputPath,
      $source.FullName
    )
  }
}

function Assemble-CompilerObjects {
  param(
    [string]$AssemblyDirectory,
    [string]$ObjectDirectory
  )

  New-Item -ItemType Directory -Force $ObjectDirectory | Out-Null
  Get-ChildItem -Path $AssemblyDirectory -Filter "*.asm" | Sort-Object Name | ForEach-Object {
    $outputPath = Join-Path $ObjectDirectory ($_.BaseName + ".obj")
    Invoke-Checked "assemble $($_.Name)" "nasm" @(
      "-f",
      "win64",
      $_.FullName,
      "-o",
      $outputPath
    )
  }
}

function Link-CompilerExecutable {
  param(
    [string]$ObjectDirectory,
    [string]$OutputPath,
    [string]$RepoRoot
  )

  $runtimeObject = Join-Path $ObjectDirectory "fcc_bootstrap_runtime.obj"
  Invoke-Checked "compile bootstrap runtime" "clang-cl" @(
    "@$RepoRoot\scripts\clang_cl_common.rsp",
    "@$RepoRoot\scripts\clang_cl_debug.rsp",
    "/c",
    "$RepoRoot\tests\toolchain\fcc_bootstrap_runtime.c",
    "/Fo$runtimeObject"
  )

  $objects = Get-ChildItem -Path $ObjectDirectory -Filter "*.obj" | Sort-Object Name |
    ForEach-Object { $_.FullName }
  $linkArguments = @(
    "@$RepoRoot\scripts\clang_cl_common.rsp",
    "@$RepoRoot\scripts\clang_cl_debug.rsp"
  )
  $linkArguments += $objects
  $linkArguments += @(
    "/Fe$OutputPath",
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
  Invoke-Checked "link $OutputPath" "clang-cl" $linkArguments
}

function Test-FilesEqual {
  param(
    [string]$LeftPath,
    [string]$RightPath
  )

  $leftBytes = [System.IO.File]::ReadAllBytes($LeftPath)
  $rightBytes = [System.IO.File]::ReadAllBytes($RightPath)
  if ($leftBytes.Length -ne $rightBytes.Length) {
    return $false
  }

  for ($byteIndex = 0; $byteIndex -lt $leftBytes.Length; $byteIndex += 1) {
    if ($leftBytes[$byteIndex] -ne $rightBytes[$byteIndex]) {
      return $false
    }
  }

  return $true
}

function Compare-AssemblyDirectories {
  param(
    [string]$LeftDirectory,
    [string]$RightDirectory
  )

  $differences = 0
  Get-ChildItem -Path $LeftDirectory -Filter "*.asm" | Sort-Object Name | ForEach-Object {
    $rightPath = Join-Path $RightDirectory $_.Name
    if (!(Test-Path $rightPath)) {
      Write-Host "missing $rightPath"
      $differences += 1
      return
    }

    if (!(Test-FilesEqual $_.FullName $rightPath)) {
      Write-Host "different $($_.Name)"
      $differences += 1
    }
  }

  if ($differences -ne 0) {
    throw "stage1/stage2 assembly comparison found $differences difference(s)"
  }
}

$repoRoot = (Resolve-Path "$PSScriptRoot\..").Path
$compilerPath = Resolve-Path $Compiler -ErrorAction SilentlyContinue
if ($null -eq $compilerPath) {
  Write-Error "FCC compiler was not found at '$Compiler'. Run 'make debug' first."
}

$sysrootPath = Resolve-Path $Sysroot -ErrorAction SilentlyContinue
if ($null -eq $sysrootPath) {
  Write-Error "FCC sysroot was not found at '$Sysroot'."
}

$buildPath = (New-Item -ItemType Directory -Force $BuildRoot).FullName
$stage0Asm = Join-Path $buildPath "stage0-asm"
$stage0Obj = Join-Path $buildPath "stage0-obj"
$stage1Dir = Join-Path $buildPath "stage1"
$stage1Asm = Join-Path $buildPath "stage1-asm"
$stage1Obj = Join-Path $buildPath "stage1-obj"
$stage2Dir = Join-Path $buildPath "stage2"
$stage2Asm = Join-Path $buildPath "stage2-asm"
$stage1Exe = Join-Path $stage1Dir "fcc_stage1.exe"
$stage2Exe = Join-Path $stage2Dir "fcc_stage2.exe"

New-Item -ItemType Directory -Force $stage1Dir, $stage2Dir | Out-Null
$sources = Get-ChildItem -Path "$repoRoot\src" -Filter "*.c" | Sort-Object Name

Push-Location $repoRoot
try {
  Emit-CompilerAssembly $compilerPath $sources $stage0Asm $sysrootPath
  Assemble-CompilerObjects $stage0Asm $stage0Obj
  Link-CompilerExecutable $stage0Obj $stage1Exe $repoRoot

  Emit-CompilerAssembly $stage1Exe $sources $stage1Asm $sysrootPath
  Assemble-CompilerObjects $stage1Asm $stage1Obj
  Link-CompilerExecutable $stage1Obj $stage2Exe $repoRoot

  Emit-CompilerAssembly $stage2Exe $sources $stage2Asm $sysrootPath
  Compare-AssemblyDirectories $stage1Asm $stage2Asm
} finally {
  Pop-Location
}

Write-Host "Self-host bootstrap succeeded."
