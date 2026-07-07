; FCC Phase 5 NASM x86_64 output for Windows 11
bits 64
default rel

global select_value
global main

section .text
select_value:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  mov rax, rcx
  mov dword [rbp - 8], eax
  xor eax, eax
  push rax
  lea rax, [rbp - 16]
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
  mov eax, dword [rbp - 8]
  cmp eax, 0
  je .L1
  cmp eax, 1
  je .L2
  cmp eax, 2
  je .L3
  jmp .L4
.L1:
  mov eax, 11
  push rax
  lea rax, [rbp - 16]
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
  jmp .L5
.L2:
  mov eax, 22
  push rax
  lea rax, [rbp - 16]
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
  jmp .L5
.L3:
  mov eax, 30
  push rax
  lea rax, [rbp - 16]
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
.L4:
  mov eax, dword [rbp - 16]
  push rax
  mov eax, 3
  mov ecx, eax
  pop rax
  add eax, ecx
  push rax
  lea rax, [rbp - 16]
  mov rcx, rax
  pop rax
  mov dword [rcx], eax
  jmp .L5
.L5:
  mov eax, dword [rbp - 16]
  jmp .L0
  xor eax, eax
.L0:
  mov rsp, rbp
  pop rbp
  ret

main:
  push rbp
  mov rbp, rsp
  sub rsp, 8
  xor eax, eax
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call select_value
  add rsp, 48
  push rax
  sub rsp, 8
  mov eax, 1
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call select_value
  add rsp, 48
  mov ecx, eax
  pop rax
  add eax, ecx
  push rax
  sub rsp, 8
  mov eax, 2
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call select_value
  add rsp, 48
  mov ecx, eax
  pop rax
  add eax, ecx
  push rax
  sub rsp, 8
  mov eax, 42
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call select_value
  add rsp, 48
  mov ecx, eax
  pop rax
  add eax, ecx
  push rax
  mov eax, 69
  mov ecx, eax
  pop rax
  sub eax, ecx
  jmp .L0
  xor eax, eax
.L0:
  mov rsp, rbp
  pop rbp
  ret

