; FCC Phase 5 NASM x86_64 output for Windows 11
bits 64
default rel

global main

section .text
main:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  lea rax, [rbp - 8]
  mov r8, rax
  mov r9, 8
.L1:
  cmp r9, 0
  je .L2
  mov byte [r8], 0
  inc r8
  dec r9
  jmp .L1
.L2:
  xor eax, eax
  movsxd rax, eax
  mov dword [rbp - 8], eax
  mov eax, 3
  movsxd rax, eax
  push rax
  lea rax, [rbp - 8]
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
  mov eax, 4
  movsxd rax, eax
  push rax
  lea rax, [rbp - 8]
  add rax, 4
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
  lea rax, [rbp - 8]
  push rax
  lea rax, [rbp - 16]
  mov rcx, rax
  pop rax
  mov rdx, rax
  mov r8, rcx
  mov r9, 8
.L3:
  cmp r9, 0
  je .L4
  mov al, byte [rdx]
  mov byte [r8], al
  inc rdx
  inc r8
  dec r9
  jmp .L3
.L4:
  lea rax, [rbp - 16]
  mov eax, dword [rax]
  movsxd rax, eax
  push rax
  lea rax, [rbp - 16]
  add rax, 4
  mov eax, dword [rax]
  movsxd rax, eax
  mov ecx, eax
  pop rax
  add eax, ecx
  movsxd rax, eax
  jmp .L0
  xor eax, eax
.L0:
  mov rsp, rbp
  pop rbp
  ret

