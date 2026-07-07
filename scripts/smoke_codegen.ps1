# SPDX-License-Identifier: GPL-3.0-or-later
param()

$ErrorActionPreference = "Stop"

Write-Host "Running FCC codegen smoke test..."
& "$PSScriptRoot\..\build\bin\fcc.exe" --emit-asm --output "$PSScriptRoot\..\build\codegen_answer.s" `
  "$PSScriptRoot\..\tests\fixtures\codegen_answer.c"
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

& "$PSScriptRoot\..\build\bin\fcc.exe" --emit-asm --output "$PSScriptRoot\..\build\codegen_answer_harness.s" `
  "$PSScriptRoot\..\tests\toolchain\codegen_answer_harness.c"
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

nasm -f win64 -o "$PSScriptRoot\..\build\codegen_answer.obj" "$PSScriptRoot\..\build\codegen_answer.s"
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

nasm -f win64 -o "$PSScriptRoot\..\build\codegen_answer_harness.obj" `
  "$PSScriptRoot\..\build\codegen_answer_harness.s"
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

clang-cl /nologo /std:c17 /TC "$PSScriptRoot\..\tests\toolchain\link_runtime_stub.c" `
  "$PSScriptRoot\..\build\codegen_answer_harness.obj" "$PSScriptRoot\..\build\codegen_answer.obj" `
  /Fe"$PSScriptRoot\..\build\codegen_answer_smoke.exe" `
  /link /nologo
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

& "$PSScriptRoot\..\build\codegen_answer_smoke.exe"
exit $LASTEXITCODE
