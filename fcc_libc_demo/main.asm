; FCC Phase 5 NASM x86_64 output for Windows 11
bits 64
default rel

extern stdin
extern stdout
extern stderr
extern fclose
extern fflush
extern fgetc
extern fputc
extern fputs
extern fprintf
extern snprintf
extern sprintf
extern printf
extern fopen
extern fopen_s
extern tmpfile
extern tmpfile_s
extern remove
extern fseek
extern ftell
extern fread
extern fwrite
extern ferror
extern abort
extern exit
extern free
extern calloc
extern malloc
extern realloc
extern memcpy
extern memmove
extern memset
extern memcmp
extern strcmp
extern _stricmp
extern strncmp
extern strstr
extern strchr
extern strlen
global main

section .rdata
FCC_STR0:
  db 109, 105, 115, 109, 97, 116, 99, 104, 58, 32, 39, 37, 115, 39, 32, 33, 61, 32, 39, 37, 115, 39, 10, 0

FCC_STR1:
  db 70, 67, 67, 32, 108, 105, 98, 99, 32, 100, 101, 109, 111, 32, 118, 97, 108, 117, 101, 61, 37, 100, 0

FCC_STR2:
  db 109, 101, 115, 115, 97, 103, 101, 58, 32, 37, 115, 10, 0

FCC_STR3:
  db 109, 101, 115, 115, 97, 103, 101, 32, 108, 101, 110, 103, 116, 104, 58, 32, 37, 108, 108, 117, 10, 0

FCC_STR4:
  db 109, 97, 108, 108, 111, 99, 32, 102, 97, 105, 108, 101, 100, 10, 0

FCC_STR5:
  db 104, 101, 97, 112, 32, 116, 101, 120, 116, 58, 32, 37, 115, 0

FCC_STR6:
  db 111, 107, 0

FCC_STR7:
  db 37, 115, 10, 0

FCC_STR8:
  db 116, 109, 112, 102, 105, 108, 101, 32, 102, 97, 105, 108, 101, 100, 10, 0

FCC_STR9:
  db 102, 119, 114, 105, 116, 101, 32, 102, 97, 105, 108, 101, 100, 10, 0

FCC_STR10:
  db 102, 115, 101, 101, 107, 32, 102, 97, 105, 108, 101, 100, 10, 0

FCC_STR11:
  db 116, 109, 112, 102, 105, 108, 101, 32, 114, 101, 97, 100, 58, 32, 37, 115, 10, 0

FCC_STR12:
  db 98, 121, 116, 101, 115, 32, 114, 101, 97, 100, 58, 32, 37, 108, 108, 117, 10, 0

section .text
require_equal:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  push rcx
  push rdx
  mov rax, qword [rsp + 8]
  mov qword [rbp - 8], rax
  mov rax, qword [rsp + 0]
  mov qword [rbp - 16], rax
  add rsp, 16
  mov rax, qword [rbp - 16]
  push rax
  mov rax, qword [rbp - 8]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  call strcmp
  add rsp, 48
  movsxd rax, eax
  push rax
  xor eax, eax
  movsxd rax, eax
  mov ecx, eax
  pop rax
  cmp eax, ecx
  setne al
  movzx eax, al
  cmp eax, 0
  je .L1
  sub rsp, 8
  mov rax, qword [rbp - 16]
  push rax
  mov rax, qword [rbp - 8]
  push rax
  lea rax, [rel FCC_STR0]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  call printf
  add rsp, 64
  mov eax, 1
  movsxd rax, eax
  jmp .L0
.L1:
  xor eax, eax
  movsxd rax, eax
  jmp .L0
  xor eax, eax
.L0:
  mov rsp, rbp
  pop rbp
  ret

main:
  push rbp
  mov rbp, rsp
  sub rsp, 432
  sub rsp, 8
  mov eax, 128
  push rax
  xor eax, eax
  push rax
  lea rax, [rbp - 128]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  call memset
  add rsp, 64
  sub rsp, 8
  mov eax, 128
  push rax
  xor eax, eax
  push rax
  lea rax, [rbp - 256]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  call memset
  add rsp, 64
  sub rsp, 8
  mov eax, 128
  push rax
  xor eax, eax
  push rax
  lea rax, [rbp - 384]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  call memset
  add rsp, 64
  mov eax, 42
  push rax
  lea rax, [rel FCC_STR1]
  push rax
  mov eax, 128
  push rax
  lea rax, [rbp - 128]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  mov r9, qword [rsp + 56]
  call snprintf
  add rsp, 64
  sub rsp, 8
  lea rax, [rbp - 128]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call strlen
  add rsp, 48
  push rax
  lea rax, [rbp - 408]
  mov rcx, rax
  pop rax
  mov qword [rcx], rax
  lea rax, [rbp - 128]
  push rax
  lea rax, [rel FCC_STR2]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  call printf
  add rsp, 48
  mov rax, qword [rbp - 408]
  push rax
  lea rax, [rel FCC_STR3]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  call printf
  add rsp, 48
  sub rsp, 8
  mov rax, qword [rbp - 408]
  push rax
  mov eax, 1
  movsxd rax, eax
  mov rcx, rax
  pop rax
  add rax, rcx
  push rax
  lea rax, [rbp - 128]
  push rax
  lea rax, [rbp - 256]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  call memcpy
  add rsp, 64
  lea rax, [rbp - 128]
  push rax
  lea rax, [rbp - 256]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  call require_equal
  add rsp, 48
  movsxd rax, eax
  push rax
  xor eax, eax
  movsxd rax, eax
  mov ecx, eax
  pop rax
  cmp eax, ecx
  setne al
  movzx eax, al
  cmp eax, 0
  je .L1
  mov eax, 1
  movsxd rax, eax
  jmp .L0
.L1:
  sub rsp, 8
  mov eax, 64
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call malloc
  add rsp, 48
  push rax
  lea rax, [rbp - 392]
  mov rcx, rax
  pop rax
  mov qword [rcx], rax
  mov rax, qword [rbp - 392]
  push rax
  xor eax, eax
  movsxd rax, eax
  mov rcx, rax
  pop rax
  cmp rax, rcx
  sete al
  movzx eax, al
  cmp eax, 0
  je .L3
  sub rsp, 8
  lea rax, [rel FCC_STR4]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call printf
  add rsp, 48
  mov eax, 1
  movsxd rax, eax
  jmp .L0
.L3:
  sub rsp, 8
  mov eax, 64
  push rax
  xor eax, eax
  push rax
  mov rax, qword [rbp - 392]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  call memset
  add rsp, 64
  sub rsp, 8
  lea rax, [rel FCC_STR6]
  push rax
  lea rax, [rel FCC_STR5]
  push rax
  mov rax, qword [rbp - 392]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  call sprintf
  add rsp, 64
  mov rax, qword [rbp - 392]
  push rax
  lea rax, [rel FCC_STR7]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  call printf
  add rsp, 48
  sub rsp, 32
  call tmpfile
  add rsp, 32
  push rax
  lea rax, [rbp - 400]
  mov rcx, rax
  pop rax
  mov qword [rcx], rax
  mov rax, qword [rbp - 400]
  push rax
  xor eax, eax
  movsxd rax, eax
  mov rcx, rax
  pop rax
  cmp rax, rcx
  sete al
  movzx eax, al
  cmp eax, 0
  je .L5
  sub rsp, 8
  lea rax, [rel FCC_STR8]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call printf
  add rsp, 48
  sub rsp, 8
  mov rax, qword [rbp - 392]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call free
  add rsp, 48
  mov eax, 1
  movsxd rax, eax
  jmp .L0
.L5:
  mov rax, qword [rbp - 400]
  push rax
  mov rax, qword [rbp - 408]
  push rax
  mov eax, 1
  push rax
  lea rax, [rbp - 128]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  mov r9, qword [rsp + 56]
  call fwrite
  add rsp, 64
  push rax
  lea rax, [rbp - 416]
  mov rcx, rax
  pop rax
  mov qword [rcx], rax
  mov rax, qword [rbp - 416]
  push rax
  mov rax, qword [rbp - 408]
  mov rcx, rax
  pop rax
  cmp rax, rcx
  setne al
  movzx eax, al
  cmp eax, 0
  je .L7
  sub rsp, 8
  lea rax, [rel FCC_STR9]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call printf
  add rsp, 48
  sub rsp, 8
  mov rax, qword [rbp - 400]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call fclose
  add rsp, 48
  sub rsp, 8
  mov rax, qword [rbp - 392]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call free
  add rsp, 48
  mov eax, 1
  movsxd rax, eax
  jmp .L0
.L7:
  sub rsp, 8
  xor eax, eax
  push rax
  xor eax, eax
  push rax
  mov rax, qword [rbp - 400]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  call fseek
  add rsp, 64
  movsxd rax, eax
  push rax
  xor eax, eax
  movsxd rax, eax
  mov ecx, eax
  pop rax
  cmp eax, ecx
  setne al
  movzx eax, al
  cmp eax, 0
  je .L9
  sub rsp, 8
  lea rax, [rel FCC_STR10]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call printf
  add rsp, 48
  sub rsp, 8
  mov rax, qword [rbp - 400]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call fclose
  add rsp, 48
  sub rsp, 8
  mov rax, qword [rbp - 392]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call free
  add rsp, 48
  mov eax, 1
  movsxd rax, eax
  jmp .L0
.L9:
  mov rax, qword [rbp - 400]
  push rax
  mov rax, qword [rbp - 408]
  push rax
  mov eax, 1
  push rax
  lea rax, [rbp - 384]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  mov r8, qword [rsp + 48]
  mov r9, qword [rsp + 56]
  call fread
  add rsp, 64
  push rax
  lea rax, [rbp - 424]
  mov rcx, rax
  pop rax
  mov qword [rcx], rax
  sub rsp, 8
  mov rax, qword [rbp - 400]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call fclose
  add rsp, 48
  lea rax, [rbp - 384]
  push rax
  lea rax, [rel FCC_STR11]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  call printf
  add rsp, 48
  mov rax, qword [rbp - 424]
  push rax
  lea rax, [rel FCC_STR12]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  call printf
  add rsp, 48
  lea rax, [rbp - 128]
  push rax
  lea rax, [rbp - 384]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  mov rdx, qword [rsp + 40]
  call require_equal
  add rsp, 48
  movsxd rax, eax
  push rax
  xor eax, eax
  movsxd rax, eax
  mov ecx, eax
  pop rax
  cmp eax, ecx
  setne al
  movzx eax, al
  cmp eax, 0
  je .L11
  sub rsp, 8
  mov rax, qword [rbp - 392]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call free
  add rsp, 48
  mov eax, 1
  movsxd rax, eax
  jmp .L0
.L11:
  sub rsp, 8
  mov rax, qword [rbp - 392]
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call free
  add rsp, 48
  sub rsp, 8
  xor eax, eax
  push rax
  sub rsp, 32
  mov rcx, qword [rsp + 32]
  call fflush
  add rsp, 48
  xor eax, eax
  movsxd rax, eax
  jmp .L0
  xor eax, eax
.L0:
  mov rsp, rbp
  pop rbp
  ret

