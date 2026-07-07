typedef void* HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;

#define STD_OUTPUT_HANDLE ((DWORD) - 11)

__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD nStdHandle);

__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE hFile, const void* lpBuffer,
                                               DWORD nNumberOfBytesToWrite,
                                               DWORD* lpNumberOfBytesWritten, void* lpOverlapped);

__declspec(dllimport) void __stdcall ExitProcess(unsigned int uExitCode);

int puts(const char* s) {
  HANDLE out;
  DWORD len = 0;
  DWORD written;

  while (s[len] != '\0')
    len++;

  out = GetStdHandle(STD_OUTPUT_HANDLE);

  if (!WriteFile(out, s, len, &written, 0))
    return -1;

  if (!WriteFile(out, "\r\n", 2, &written, 0))
    return -1;

  return 0;
}

void mainCRTStartup(void) {
  for (;;) {
    puts("Hello world");
  }

  ExitProcess(0);
}
