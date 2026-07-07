/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void* HANDLE;
typedef int BOOL;
#ifndef FCC_BYTE_TYPE_DEFINED
typedef unsigned char BYTE;
#define FCC_BYTE_TYPE_DEFINED 1
#endif
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long long ULONGLONG;

typedef struct LARGE_INTEGER {
  long long QuadPart;
} LARGE_INTEGER;

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_NAME 123

#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 8

#define GENERIC_READ 0x80000000U
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 128
#define INVALID_HANDLE_VALUE ((HANDLE)(-1))

int MultiByteToWideChar(...);
HANDLE CreateFileW(...);
BOOL ReadFile(...);
BOOL CloseHandle(...);
BOOL DeleteFileW(...);
BOOL GetFileSizeEx(...);
DWORD GetLastError(...);

#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_EXECUTABLE_IMAGE 0x0002
#define IMAGE_FILE_LARGE_ADDRESS_AWARE 0x0020
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20B
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 3
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

#define IMAGE_SCN_CNT_CODE 0x00000020
#define IMAGE_SCN_CNT_INITIALIZED_DATA 0x00000040
#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 0x00000080
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000

#define IMAGE_SYM_UNDEFINED 0
#define IMAGE_SYM_CLASS_EXTERNAL 2

#define IMAGE_REL_AMD64_ADDR64 0x0001
#define IMAGE_REL_AMD64_ADDR32 0x0002
#define IMAGE_REL_AMD64_ADDR32NB 0x0003
#define IMAGE_REL_AMD64_REL32 0x0004
#define IMAGE_REL_AMD64_REL32_1 0x0005
#define IMAGE_REL_AMD64_REL32_2 0x0006
#define IMAGE_REL_AMD64_REL32_3 0x0007
#define IMAGE_REL_AMD64_REL32_4 0x0008
#define IMAGE_REL_AMD64_REL32_5 0x0009

typedef struct IMAGE_DOS_HEADER {
  WORD e_magic;
  WORD e_cblp;
  WORD e_cp;
  WORD e_crlc;
  WORD e_cparhdr;
  WORD e_minalloc;
  WORD e_maxalloc;
  WORD e_ss;
  WORD e_sp;
  WORD e_csum;
  WORD e_ip;
  WORD e_cs;
  WORD e_lfarlc;
  WORD e_ovno;
  WORD e_res[4];
  WORD e_oemid;
  WORD e_oeminfo;
  WORD e_res2[10];
  LONG e_lfanew;
} IMAGE_DOS_HEADER;

typedef struct IMAGE_FILE_HEADER {
  WORD Machine;
  WORD NumberOfSections;
  DWORD TimeDateStamp;
  DWORD PointerToSymbolTable;
  DWORD NumberOfSymbols;
  WORD SizeOfOptionalHeader;
  WORD Characteristics;
} IMAGE_FILE_HEADER;

typedef union IMAGE_SECTION_HEADER_MISC {
  DWORD PhysicalAddress;
  DWORD VirtualSize;
} IMAGE_SECTION_HEADER_MISC;

typedef struct IMAGE_SECTION_HEADER {
  BYTE Name[8];
  IMAGE_SECTION_HEADER_MISC Misc;
  DWORD VirtualAddress;
  DWORD SizeOfRawData;
  DWORD PointerToRawData;
  DWORD PointerToRelocations;
  DWORD PointerToLinenumbers;
  WORD NumberOfRelocations;
  WORD NumberOfLinenumbers;
  DWORD Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct IMAGE_RELOCATION {
  DWORD VirtualAddress;
  DWORD SymbolTableIndex;
  WORD Type;
} IMAGE_RELOCATION;

typedef struct IMAGE_SYMBOL_NAME_OFFSET {
  DWORD Short;
  DWORD Long;
} IMAGE_SYMBOL_NAME_OFFSET;

typedef union IMAGE_SYMBOL_NAME {
  BYTE ShortName[8];
  IMAGE_SYMBOL_NAME_OFFSET Name;
} IMAGE_SYMBOL_NAME;

typedef struct IMAGE_SYMBOL {
  IMAGE_SYMBOL_NAME N;
  DWORD Value;
  short SectionNumber;
  WORD Type;
  BYTE StorageClass;
  BYTE NumberOfAuxSymbols;
} IMAGE_SYMBOL;

typedef struct IMAGE_OPTIONAL_HEADER64 {
  WORD Magic;
  BYTE MajorLinkerVersion;
  BYTE MinorLinkerVersion;
  DWORD SizeOfCode;
  DWORD SizeOfInitializedData;
  DWORD SizeOfUninitializedData;
  DWORD AddressOfEntryPoint;
  DWORD BaseOfCode;
  uint64_t ImageBase;
  DWORD SectionAlignment;
  DWORD FileAlignment;
  WORD MajorOperatingSystemVersion;
  WORD MinorOperatingSystemVersion;
  WORD MajorImageVersion;
  WORD MinorImageVersion;
  WORD MajorSubsystemVersion;
  WORD MinorSubsystemVersion;
  DWORD Win32VersionValue;
  DWORD SizeOfImage;
  DWORD SizeOfHeaders;
  DWORD CheckSum;
  WORD Subsystem;
  WORD DllCharacteristics;
  uint64_t SizeOfStackReserve;
  uint64_t SizeOfStackCommit;
  uint64_t SizeOfHeapReserve;
  uint64_t SizeOfHeapCommit;
  DWORD LoaderFlags;
  DWORD NumberOfRvaAndSizes;
} IMAGE_OPTIONAL_HEADER64;
