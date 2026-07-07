; FCC Phase 5 NASM x86_64 output for Windows 11
bits 64
default rel

global main

section .text
main:
  push rbp
  mov rbp, rsp
  sub rsp, 32
  mov eax, 1
  mov dword [rbp - 8], eax
  lea rax, [rbp - 24]
  mov qword [rbp - 32], rax
  lea rax, [rbp - 8]
  push rax
  mov eax, dword [rax]
  push rax
  mov eax, 2
  mov rcx, rax
  pop rax
  add eax, ecx
  pop rcx
  mov dword [rcx], eax
  lea rax, [rbp - 8]
  push rax
  mov eax, dword [rax]
  push rax
  mov eax, 3
  mov rcx, rax
  pop rax
  imul eax, ecx
  pop rcx
  mov dword [rcx], eax
  lea rax, [rbp - 8]
  push rax
  mov eax, dword [rax]
  push rax
  mov eax, 1
  mov rcx, rax
  pop rax
  sub eax, ecx
  pop rcx
  mov dword [rcx], eax
  lea rax, [rbp - 8]
  push rax
  mov eax, dword [rax]
  add eax, 1
  pop rcx
  mov dword [rcx], eax
  lea rax, [rbp - 8]
  push rax
  mov eax, dword [rax]
  push rax
  sub eax, 1
  pop rdx
  pop rcx
  mov dword [rcx], eax
  mov rax, rdx
  mov eax, 3
  push rax
  lea rax, [rbp - 24]
  push rax
  xor eax, eax
  imul rax, rax, 4
  pop rcx
  add rax, rcx
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
  mov eax, 4
  push rax
  lea rax, [rbp - 24]
  push rax
  mov eax, 1
  imul rax, rax, 4
  pop rcx
  add rax, rcx
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
  lea rax, [rbp - 32]
  push rax
  mov rax, qword [rax]
  push rax
  add rax, 4
  pop rdx
  pop rcx
  mov qword [rcx], rax
  mov rax, rdx
  mov rax, qword [rbp - 32]
  push rax
  xor eax, eax
  imul rax, rax, 4
  pop rcx
  add rax, rcx
  push rax
  mov eax, dword [rax]
  push rax
  mov eax, dword [rbp - 8]
  mov rcx, rax
  pop rax
  add eax, ecx
  pop rcx
  mov dword [rcx], eax
  lea rax, [rbp - 32]
  push rax
  mov rax, qword [rax]
  sub rax, 4
  pop rcx
  mov qword [rcx], rax
  lea rax, [rbp - 32]
  push rax
  mov rax, qword [rax]
  push rax
  mov eax, 2
  mov rcx, rax
  pop rax
  imul rcx, rcx, 4
  add rax, rcx
  pop rcx
  mov qword [rcx], rax
  lea rax, [rbp - 32]
  push rax
  mov rax, qword [rax]
  push rax
  sub rax, 4
  pop rdx
  pop rcx
  mov qword [rcx], rax
  mov rax, rdx
  lea rax, [rbp - 24]
  push rax
  xor eax, eax
  imul rax, rax, 4
  pop rcx
  add rax, rcx
  mov eax, dword [rax]
  push rax
  lea rax, [rbp - 24]
  push rax
  mov eax, 1
  imul rax, rax, 4
  pop rcx
  add rax, rcx
  mov eax, dword [rax]
  mov ecx, eax
  pop rax
  add eax, ecx
  push rax
  mov rax, qword [rbp - 32]
  push rax
  xor eax, eax
  imul rax, rax, 4
  pop rcx
  add rax, rcx
  mov eax, dword [rax]
  mov ecx, eax
  pop rax
  add eax, ecx
  push rax
  mov eax, 27
  mov ecx, eax
  pop rax
  sub eax, ecx
  jmp .L0
  xor eax, eax
.L0:
  mov rsp, rbp
  pop rbp
  ret

