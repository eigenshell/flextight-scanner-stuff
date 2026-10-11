/*
 * Minimal Win32 API stub — OFFLINE SYNTAX CHECK ONLY.
 *
 * The dev box has no mingw toolchain, so hook.c / inject.c are compile-checked
 * with `gcc -fsyntax-only -I test/win32stub`. This header declares just enough
 * of the Win32 surface for that check; it is NEVER used to build the real
 * DLL/exe (that happens with i686-w64-mingw32-gcc or cl on Windows).
 */
#ifndef FT_WIN32STUB_WINDOWS_H
#define FT_WIN32STUB_WINDOWS_H

typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef void *HANDLE;
typedef void *HMODULE;
typedef void *HINSTANCE;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef unsigned int UINT;
typedef unsigned long ULONG_PTR;
typedef unsigned long *LPDWORD;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef int (*FARPROC)(void);
typedef unsigned long SIZE_T;
typedef unsigned char BYTE;
typedef void *LPSECURITY_ATTRIBUTES;
typedef void *LPOVERLAPPED;
typedef unsigned long *PULONG;
typedef long LONG;

#define WINAPI
#define TRUE 1
#define FALSE 0
#define NULL ((void *)0)

#define MAX_PATH 260
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READWRITE 0x40
#define CREATE_SUSPENDED 0x4
#define INFINITE 0xFFFFFFFF
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 0x102
#define STILL_ACTIVE 0x103

typedef struct _STARTUPINFOA {
    DWORD cb;
    void *reserved0;
    char *lpDesktop;
    char *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    void *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOA;

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION;

typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID lpParameter);

/* CRITICAL_SECTION (stub shape only — real one lives in winnt.h) */
typedef struct _CRITICAL_SECTION {
    void *DebugInfo;
    long LockCount;
    long RecursionCount;
    HANDLE OwningThread;
    HANDLE LockSemaphore;
    ULONG_PTR SpinCount;
} CRITICAL_SECTION;
typedef CRITICAL_SECTION *LPCRITICAL_SECTION;

void InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection);

HMODULE GetModuleHandleA(LPCSTR lpModuleName);
FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
LPVOID VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType,
                    DWORD flProtect);
BOOL VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect,
                    LPDWORD lpflOldProtect);
BOOL FlushInstructionCache(HANDLE hProcess, LPCVOID lpBaseAddress, SIZE_T dwSize);

/* MEMORY_BASIC_INFORMATION + VirtualQuery (used by the payload sidecar to
 * guard dereferences of SRB data pointers) */
typedef struct _MEMORY_BASIC_INFORMATION {
    LPVOID BaseAddress;
    LPVOID AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
} MEMORY_BASIC_INFORMATION;
SIZE_T VirtualQuery(LPCVOID lpAddress, MEMORY_BASIC_INFORMATION *lpBuffer,
                    SIZE_T dwLength);

#define MEM_COMMIT 0x1000
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80

HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
DWORD GetTickCount(void);
BOOL DisableThreadLibraryCalls(HMODULE hModule);
BOOL CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine,
                    LPSECURITY_ATTRIBUTES lpProcessAttributes,
                    LPSECURITY_ATTRIBUTES lpThreadAttributes,
                    BOOL bInheritHandles, DWORD dwCreationFlags,
                    LPVOID lpEnvironment, LPCSTR lpCurrentDirectory,
                    STARTUPINFOA *lpStartupInfo,
                    PROCESS_INFORMATION *lpProcessInformation);
DWORD GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize);
DWORD GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize);
LPVOID VirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize,
                      DWORD flAllocationType, DWORD flProtect);
BOOL WriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer,
                        SIZE_T nSize, SIZE_T *lpNumberOfBytesWritten);
HANDLE CreateRemoteThread(HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                          SIZE_T dwStackSize,
                          LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
                          DWORD dwCreationFlags, LPDWORD lpThreadId);
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
BOOL GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);
DWORD ResumeThread(HANDLE hThread);
BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode);
BOOL CloseHandle(HANDLE hObject);
DWORD GetLastError(void);

#endif /* FT_WIN32STUB_WINDOWS_H */
