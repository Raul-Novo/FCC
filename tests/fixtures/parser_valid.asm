; FCC Phase 5 NASM x86_64 output for Windows 11
bits 64
default rel

global sum_to
global spin

section .text
sum_to:
  push rbp
  mov rbp, rsp
  sub rsp, 32
  mov rax, rcx
  mov dword [rbp - 8], eax
  xor eax, eax
  mov dword [rbp - 16], eax
  xor eax, eax
  mov dword [rbp - 24], eax
.L1:
  mov eax, dword [rbp - 24]
  push rax
  mov eax, dword [rbp - 8]
  mov ecx, eax
  pop rax
  cmp eax, ecx
  setl al
  movzx eax, al
  cmp eax, 0
  je .L2
  mov eax, dword [rbp - 16]
  push rax
  mov eax, dword [rbp - 24]
  mov ecx, eax
  pop rax
  add eax, ecx
  mov dword [rbp - 16], eax
  mov eax, dword [rbp - 24]
  push rax
  mov eax, 1
  mov ecx, eax
  pop rax
  add eax, ecx
  mov dword [rbp - 24], eax
  jmp .L1
.L2:
  mov eax, dword [rbp - 16]
  jmp .L0
  xor eax, eax
.L0:
  mov rsp, rbp
  pop rbp
  ret

spin:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  xor eax, eax
  mov dword [rbp - 8], eax
  xor eax, eax
  mov dword [rbp - 8], eax
.L1:
  mov eax, dword [rbp - 8]
  push rax
  mov eax, 4
  mov ecx, eax
  pop rax
  cmp eax, ecx
  setl al
  movzx eax, al
  cmp eax, 0
  je .L3
  mov eax, dword [rbp - 8]
  push rax
  mov eax, 2
  mov ecx, eax
  pop rax
  cmp eax, ecx
  sete al
  movzx eax, al
  cmp eax, 0
  je .L4
  jmp .L2
  jmp .L5
.L4:
  jmp .L3
.L5:
.L2:
  mov eax, dword [rbp - 8]
  push rax
  mov eax, 1
  mov ecx, eax
  pop rax
  add eax, ecx
  mov dword [rbp - 8], eax
  jmp .L1
.L3:
  xor eax, eax
  jmp .L0
  xor eax, eax
.L0:
  mov rsp, rbp
  pop rbp
  ret

