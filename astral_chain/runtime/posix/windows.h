/* Linux host: the subset of the Win32 API the Astral Chain runtime uses, implemented on POSIX and SDL3
 * (win32_posix.c). Only Linux builds put this folder on the include path, so the runtime keeps its Windows code and
 * includes <windows.h> unchanged. Types keep their Windows widths (LONG and DWORD are 32-bit). wchar_t is 32-bit
 * here: wide strings hold UTF-16 code units (surrogate pairs included) and are converted to UTF-8 at the OS boundary;
 * paths have "\\?\" removed and backslashes turned into slashes. */
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include <strings.h>
#include <sys/types.h>

#define WIN32_POSIX 1
#define WINAPI
#define CALLBACK
#define APIENTRY
#define WINBASEAPI
#define DECLSPEC_IMPORT
#define __stdcall
#define __cdecl
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
#define CONST const
#define VOID void

typedef int BOOL;
typedef unsigned char BYTE, UCHAR, BOOLEAN, *PUCHAR, *PBYTE, *LPBYTE;
typedef uint16_t WORD, USHORT;
typedef int16_t SHORT;
typedef uint32_t DWORD, ULONG, UINT, *LPDWORD, *PDWORD, *PULONG;
typedef int32_t LONG, INT, HRESULT, NTSTATUS, *PLONG;
typedef int64_t LONG64, LONGLONG, INT64, *PLONG64;
typedef uint64_t ULONG64, ULONGLONG, DWORD64, UINT64, DWORDLONG;
typedef uintptr_t ULONG_PTR, DWORD_PTR, UINT_PTR, *PULONG_PTR, *PDWORD_PTR;
typedef intptr_t LONG_PTR, INT_PTR, LPARAM, LRESULT;
typedef uintptr_t WPARAM;
typedef size_t SIZE_T, *PSIZE_T;
typedef void *PVOID, *LPVOID, *HANDLE;
typedef HANDLE *PHANDLE, *LPHANDLE;
typedef const void *LPCVOID;
typedef char CHAR, *LPSTR, *PSTR;
typedef const char *LPCSTR, *PCSTR;
typedef wchar_t WCHAR, *LPWSTR, *PWSTR;
typedef const wchar_t *LPCWSTR, *PCWSTR;
typedef WORD ATOM;
typedef DWORD COLORREF;
typedef HANDLE HMODULE, HINSTANCE, HWND, HDC, HBRUSH, HCURSOR, HICON, HMENU, HFONT, HBITMAP, HGDIOBJ, HMONITOR, HRAWINPUT,
 HLOCAL, HGLOBAL, HKEY;
typedef intptr_t (*FARPROC)(void);

typedef union { struct { DWORD LowPart; LONG HighPart; }; struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; }
 LARGE_INTEGER, *PLARGE_INTEGER;
typedef union { struct { DWORD LowPart; DWORD HighPart; }; struct { DWORD LowPart; DWORD HighPart; } u; ULONGLONG QuadPart; }
 ULARGE_INTEGER, *PULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *LPSYSTEMTIME;
typedef struct { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;
typedef struct { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef struct { LONG x, y; } POINT, *LPPOINT;
typedef struct { LONG cx, cy; } SIZE, *LPSIZE;

#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define MAKEINTRESOURCEW(i) ((LPWSTR)(ULONG_PTR)(WORD)(i))
#define MAKEINTRESOURCEA(i) ((LPSTR)(ULONG_PTR)(WORD)(i))
#define UNREFERENCED_PARAMETER(p) (void)(p)
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------------------------------------ errors */
#define ERROR_SUCCESS 0u
#define NO_ERROR 0u
#define ERROR_INVALID_FUNCTION 1u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_TOO_MANY_OPEN_FILES 4u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_DATA 13u
#define ERROR_OUTOFMEMORY 14u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_WRITE_FAULT 29u
#define ERROR_READ_FAULT 30u
#define ERROR_SHARING_VIOLATION 32u
#define ERROR_LOCK_VIOLATION 33u
#define ERROR_HANDLE_EOF 38u
#define ERROR_NOT_SUPPORTED 50u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_BROKEN_PIPE 109u
#define ERROR_DISK_FULL 112u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_INVALID_NAME 123u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_PROC_NOT_FOUND 127u
#define ERROR_DIR_NOT_EMPTY 145u
#define ERROR_BAD_PATHNAME 161u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_ENVVAR_NOT_FOUND 203u
#define ERROR_MORE_DATA 234u
#define ERROR_NO_UNICODE_TRANSLATION 1113u
#define ERROR_INVALID_ADDRESS 487u
#define ERROR_INVALID_STATE 5023u
#define ERROR_TIMEOUT 1460u
DWORD GetLastError(void);
void SetLastError(DWORD error);

/* ------------------------------------------------------------------------------------------------ handles, waits */
#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED 0x80u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define WAIT_IO_COMPLETION 0xC0u
#define MAXIMUM_WAIT_OBJECTS 64
#define DUPLICATE_CLOSE_SOURCE 1u
#define DUPLICATE_SAME_ACCESS 2u
#define HANDLE_FLAG_INHERIT 1u
#define SYNCHRONIZE 0x00100000u
#define EVENT_ALL_ACCESS 0x1F0003u
#define THREAD_ALL_ACCESS 0x1FFFFFu
#define PROCESS_ALL_ACCESS 0x1FFFFFu
BOOL CloseHandle(HANDLE handle);
BOOL DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process, LPHANDLE target, DWORD access,
                     BOOL inherit, DWORD options);
BOOL SetHandleInformation(HANDLE handle, DWORD mask, DWORD flags);
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, LPCWSTR name);
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE event);
BOOL ResetEvent(HANDLE event);
DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds);
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD milliseconds);
DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable);
#define QS_ALLINPUT 0x04FFu
#define QS_ALLEVENTS 0x04BFu
#define MWMO_WAITALL 1u
#define MWMO_ALERTABLE 2u
#define MWMO_INPUTAVAILABLE 4u
DWORD MsgWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, DWORD milliseconds, DWORD wake_mask, DWORD flags);
DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD milliseconds, DWORD wake_mask);

/* SRW locks and condition variables: a pthread mutex and condition (all-zero initializers on glibc, as on Windows). */
typedef struct { pthread_mutex_t mutex; } SRWLOCK, *PSRWLOCK;
typedef struct { pthread_cond_t cond; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;
#define SRWLOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
#define CONDITION_VARIABLE_INIT {PTHREAD_COND_INITIALIZER}
#define CONDITION_VARIABLE_LOCKMODE_SHARED 1u
static inline void InitializeSRWLock(PSRWLOCK lock) { pthread_mutex_init(&lock->mutex, NULL); }
static inline void AcquireSRWLockExclusive(PSRWLOCK lock) { pthread_mutex_lock(&lock->mutex); }
static inline void ReleaseSRWLockExclusive(PSRWLOCK lock) { pthread_mutex_unlock(&lock->mutex); }
static inline BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) { return pthread_mutex_trylock(&lock->mutex) == 0; }
static inline void AcquireSRWLockShared(PSRWLOCK lock) { pthread_mutex_lock(&lock->mutex); }
static inline void ReleaseSRWLockShared(PSRWLOCK lock) { pthread_mutex_unlock(&lock->mutex); }
static inline void InitializeConditionVariable(PCONDITION_VARIABLE cv) { pthread_cond_init(&cv->cond, NULL); }
static inline void WakeConditionVariable(PCONDITION_VARIABLE cv) { pthread_cond_signal(&cv->cond); }
static inline void WakeAllConditionVariable(PCONDITION_VARIABLE cv) { pthread_cond_broadcast(&cv->cond); }
BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD milliseconds, ULONG flags);
typedef struct { pthread_mutex_t mutex; } CRITICAL_SECTION, *LPCRITICAL_SECTION;
void InitializeCriticalSection(LPCRITICAL_SECTION section);
static inline void EnterCriticalSection(LPCRITICAL_SECTION section) { pthread_mutex_lock(&section->mutex); }
static inline void LeaveCriticalSection(LPCRITICAL_SECTION section) { pthread_mutex_unlock(&section->mutex); }
static inline void DeleteCriticalSection(LPCRITICAL_SECTION section) { pthread_mutex_destroy(&section->mutex); }

/* Interlocked operations: full barriers, as on x86-64 Windows. */
#define InterlockedExchange(t, v) __atomic_exchange_n((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64(t, v) __atomic_exchange_n((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangePointer(t, v) __atomic_exchange_n((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement(t) __atomic_add_fetch((t), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(t) __atomic_sub_fetch((t), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64(t) __atomic_add_fetch((t), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement64(t) __atomic_sub_fetch((t), 1, __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd(t, v) __atomic_fetch_add((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64(t, v) __atomic_fetch_add((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd64(t, v) __atomic_add_fetch((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr(t, v) __atomic_fetch_or((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd(t, v) __atomic_fetch_and((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr64(t, v) __atomic_fetch_or((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedAnd64(t, v) __atomic_fetch_and((t), (v), __ATOMIC_SEQ_CST)
#define InterlockedCompareExchange(t, v, c) __sync_val_compare_and_swap((t), (c), (v))
#define InterlockedCompareExchange64(t, v, c) __sync_val_compare_and_swap((t), (c), (v))
#define _InterlockedCompareExchange64(t, v, c) __sync_val_compare_and_swap((t), (c), (v))
#define InterlockedCompareExchangePointer(t, v, c) __sync_val_compare_and_swap((t), (c), (v))
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() __builtin_ia32_pause()
#define _ReadWriteBarrier() __asm__ __volatile__("" ::: "memory")

/* ------------------------------------------------------------------------------------------------ threads, process */
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID parameter);
#define CREATE_SUSPENDED 0x4u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
#define THREAD_PRIORITY_IDLE (-15)
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define STILL_ACTIVE 259u
HANDLE CreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack_size, LPTHREAD_START_ROUTINE start, LPVOID parameter,
                    DWORD flags, LPDWORD thread_id);
__attribute__((noreturn)) void ExitThread(DWORD code);
DWORD ResumeThread(HANDLE thread);
DWORD SuspendThread(HANDLE thread);
BOOL GetExitCodeThread(HANDLE thread, LPDWORD code);
BOOL SetThreadPriority(HANDLE thread, int priority);
int GetThreadPriority(HANDLE thread);
DWORD GetThreadId(HANDLE thread);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
HANDLE GetCurrentProcess(void);
DWORD GetCurrentProcessId(void);
void GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high);
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask);
__attribute__((noreturn)) void ExitProcess(UINT code);
BOOL TerminateProcess(HANDLE process, UINT code);
BOOL GetExitCodeProcess(HANDLE process, LPDWORD code);
void Sleep(DWORD milliseconds);
DWORD SleepEx(DWORD milliseconds, BOOL alertable);
BOOL SwitchToThread(void);
DWORD GetActiveProcessorCount(WORD group);
#define ALL_PROCESSOR_GROUPS 0xffff
typedef struct { DWORD flags; } CONTEXT_STUB;
typedef struct {
 DWORD ContextFlags; DWORD64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15, Rip;
} CONTEXT, *PCONTEXT;
#define CONTEXT_CONTROL 0x100001u
#define CONTEXT_INTEGER 0x100002u
#define CONTEXT_FULL 0x10000Bu
BOOL GetThreadContext(HANDLE thread, PCONTEXT context);
typedef struct {
 union { DWORD dwOemId; struct { WORD wProcessorArchitecture, wReserved; }; };
 DWORD dwPageSize; LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress; DWORD_PTR dwActiveProcessorMask;
 DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity; WORD wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO, *LPSYSTEM_INFO;
void GetSystemInfo(LPSYSTEM_INFO info);
void GetNativeSystemInfo(LPSYSTEM_INFO info);

typedef struct { DWORD cb; LPSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars,
 dwYCountChars, dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2; LPBYTE lpReserved2; HANDLE hStdInput, hStdOutput,
 hStdError; } STARTUPINFOA, *LPSTARTUPINFOA;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
#define STARTF_USESTDHANDLES 0x100u
#define CREATE_NO_WINDOW 0x08000000u
/* The command line is split as the Microsoft C runtime does (quotes, backslashes); the program is looked up in PATH. */
BOOL CreateProcessA(LPCSTR application, LPSTR command_line, LPSECURITY_ATTRIBUTES process_attributes,
                    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment,
                    LPCSTR directory, LPSTARTUPINFOA startup, LPPROCESS_INFORMATION information);
BOOL CreatePipe(PHANDLE read, PHANDLE write, LPSECURITY_ATTRIBUTES attributes, DWORD size);
BOOL PeekNamedPipe(HANDLE pipe, LPVOID buffer, DWORD size, LPDWORD read, LPDWORD available, LPDWORD left);

/* ------------------------------------------------------------------------------------------------ environment, time */
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size);
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value);
BOOL QueryPerformanceCounter(LARGE_INTEGER *counter);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency);
ULONGLONG GetTickCount64(void);
DWORD GetTickCount(void);
void GetSystemTimePreciseAsFileTime(LPFILETIME time);
void GetSystemTimeAsFileTime(LPFILETIME time);
void GetLocalTime(LPSYSTEMTIME time);
void GetSystemTime(LPSYSTEMTIME time);
typedef struct { LONG Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; LONG StandardBias; WCHAR DaylightName[32];
 SYSTEMTIME DaylightDate; LONG DaylightBias; } TIME_ZONE_INFORMATION, *LPTIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_INVALID 0xFFFFFFFFu
#define TIME_ZONE_ID_UNKNOWN 0u
#define TIME_ZONE_ID_STANDARD 1u
#define TIME_ZONE_ID_DAYLIGHT 2u
DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION information);
typedef int64_t __time64_t;
static inline int _localtime64_s(struct tm *out, const __time64_t *t) { time_t v = (time_t)*t; return localtime_r(&v, out) ? 0 : 22; }
static inline int _gmtime64_s(struct tm *out, const __time64_t *t) { time_t v = (time_t)*t; return gmtime_r(&v, out) ? 0 : 22; }
static inline __time64_t _time64(__time64_t *t) { __time64_t v = (__time64_t)time(NULL); if (t) *t = v; return v; }

/* ------------------------------------------------------------------------------------------------ strings */
#define CP_ACP 0u
#define CP_UTF8 65001u
#define MB_ERR_INVALID_CHARS 8u
#define WC_ERR_INVALID_CHARS 0x80u
int MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR input, int input_size, LPWSTR output, int output_size);
int WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR input, int input_size, LPSTR output, int output_size,
                        LPCSTR default_char, BOOL *used_default);
#include "w32_crt.h"
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
int _wmkdir(const wchar_t *path);
int _wremove(const wchar_t *path);

/* ------------------------------------------------------------------------------------------------ files */
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u
#define FILE_READ_DATA 1u
#define FILE_WRITE_DATA 2u
#define FILE_APPEND_DATA 4u
#define FILE_READ_ATTRIBUTES 0x80u
#define FILE_WRITE_ATTRIBUTES 0x100u
#define DELETE 0x00010000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_HIDDEN 0x2u
#define FILE_ATTRIBUTE_SYSTEM 0x4u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_ATTRIBUTE_TEMPORARY 0x100u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400u
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define FILE_FLAG_NO_BUFFERING 0x20000000u
#define FILE_FLAG_RANDOM_ACCESS 0x10000000u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define MOVEFILE_REPLACE_EXISTING 1u
#define MOVEFILE_COPY_ALLOWED 2u
#define MOVEFILE_WRITE_THROUGH 8u
typedef struct { DWORD Internal_[2]; union { struct { DWORD Offset, OffsetHigh; }; PVOID Pointer; }; HANDLE hEvent; }
 OVERLAPPED, *LPOVERLAPPED;
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
                   DWORD flags, HANDLE template_file);
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
                   DWORD flags, HANDLE template_file);
BOOL ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped);
BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped);
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method);
DWORD SetFilePointer(HANDLE file, LONG distance, PLONG high, DWORD method);
BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size);
DWORD GetFileSize(HANDLE file, LPDWORD high);
BOOL SetEndOfFile(HANDLE file);
BOOL FlushFileBuffers(HANDLE file);
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
 DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow; }
 BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;
BOOL GetFileInformationByHandle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION information);
BOOL DeleteFileW(LPCWSTR path);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES attributes);
BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES attributes);
BOOL RemoveDirectoryW(LPCWSTR path);
BOOL RemoveDirectoryA(LPCSTR path);
DWORD GetFileAttributesW(LPCWSTR path);
DWORD GetFileAttributesA(LPCSTR path);
BOOL GetDiskFreeSpaceExW(LPCWSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes);
BOOL GetDiskFreeSpaceExA(LPCSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes);
DWORD GetTempPathA(DWORD size, LPSTR buffer);
DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer);
DWORD GetFullPathNameA(LPCSTR path, DWORD size, LPSTR buffer, LPSTR *file_part);
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh,
 nFileSizeLow, dwReserved0, dwReserved1; WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14]; }
 WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh,
 nFileSizeLow, dwReserved0, dwReserved1; CHAR cFileName[MAX_PATH]; CHAR cAlternateFileName[14]; }
 WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data);
BOOL FindNextFileW(HANDLE find, LPWIN32_FIND_DATAW data);
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data);
BOOL FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data);
BOOL FindClose(HANDLE find);
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
HANDLE GetStdHandle(DWORD which);
DWORD GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size);
DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size);

/* ------------------------------------------------------------------------------------------------ virtual memory */
#define MEM_COMMIT 0x00001000u
#define MEM_RESERVE 0x00002000u
#define MEM_REPLACE_PLACEHOLDER 0x00004000u
#define MEM_DECOMMIT 0x00004000u
#define MEM_RELEASE 0x00008000u
#define MEM_FREE 0x00010000u
#define MEM_PRIVATE 0x00020000u
#define MEM_MAPPED 0x00040000u
#define MEM_RESERVE_PLACEHOLDER 0x00040000u
#define MEM_RESET 0x00080000u
#define MEM_TOP_DOWN 0x00100000u
#define MEM_WRITE_WATCH 0x00200000u
#define MEM_LARGE_PAGES 0x20000000u
#define MEM_COALESCE_PLACEHOLDERS 0x00000001u
#define MEM_PRESERVE_PLACEHOLDER 0x00000002u
#define MEM_IMAGE 0x01000000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_WRITECOPY 0x08u
#define PAGE_EXECUTE 0x10u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_GUARD 0x100u
#define PAGE_NOCACHE 0x200u
#define WRITE_WATCH_FLAG_RESET 1u
typedef struct { PVOID BaseAddress, AllocationBase; DWORD AllocationProtect; WORD PartitionId; SIZE_T RegionSize;
 DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
typedef struct { uint64_t Type_Reserved; union { ULONG64 ULong64; PVOID Pointer; SIZE_T Size; HANDLE Handle; ULONG ULong; }; }
 MEM_EXTENDED_PARAMETER, *PMEM_EXTENDED_PARAMETER;
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect);
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T size);
UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addresses, ULONG_PTR *count, PULONG granularity);
UINT ResetWriteWatch(LPVOID base, SIZE_T size);
BOOL FlushInstructionCache(HANDLE process, LPCVOID address, SIZE_T size);
/* KernelBase placeholders (GetProcAddress on GetModuleHandleW(L"KernelBase.dll") returns these). */
PVOID VirtualAlloc2(HANDLE process, PVOID address, SIZE_T size, ULONG type, ULONG protect, MEM_EXTENDED_PARAMETER *params,
                    ULONG count);
PVOID MapViewOfFile3(HANDLE mapping, HANDLE process, PVOID address, ULONG64 offset, SIZE_T size, ULONG type, ULONG protect,
                     MEM_EXTENDED_PARAMETER *params, ULONG count);
BOOL UnmapViewOfFile2(HANDLE process, PVOID address, ULONG flags);
#define FILE_MAP_COPY 1u
#define FILE_MAP_WRITE 2u
#define FILE_MAP_READ 4u
#define FILE_MAP_ALL_ACCESS 0xF001Fu
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect, DWORD size_high, DWORD size_low,
                          LPCWSTR name);
HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect, DWORD size_high, DWORD size_low,
                          LPCSTR name);
LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T size);
BOOL UnmapViewOfFile(LPCVOID address);
HANDLE GetProcessHeap(void);
LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size);
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID memory);
#define HEAP_ZERO_MEMORY 8u
static inline HLOCAL LocalFree(HLOCAL memory) { free(memory); return NULL; }

/* ------------------------------------------------------------------------------------------------ exceptions */
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define EXCEPTION_STACK_OVERFLOW 0xC00000FDu
#define EXCEPTION_IN_PAGE_ERROR 0xC0000006u
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD { DWORD ExceptionCode, ExceptionFlags; struct _EXCEPTION_RECORD *ExceptionRecord;
 PVOID ExceptionAddress; DWORD NumberParameters; ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS]; }
 EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct { PEXCEPTION_RECORD ExceptionRecord; PCONTEXT ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS,
 *LPEXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS info);
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS info);
PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG RemoveVectoredExceptionHandler(PVOID handle);
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter);
#define SEM_FAILCRITICALERRORS 1u
#define SEM_NOGPFAULTERRORBOX 2u
static inline UINT SetErrorMode(UINT mode) { (void)mode; return 0; }
static inline BOOL IsDebuggerPresent(void) { return FALSE; }
static inline void DebugBreak(void) { __builtin_trap(); }
static inline void OutputDebugStringA(LPCSTR text) { (void)text; }

/* ------------------------------------------------------------------------------------------------ modules */
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x100u
#define LOAD_LIBRARY_SEARCH_APPLICATION_DIR 0x200u
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800u
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x1000u
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2u
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4u
/* Built-in modules: kernelbase.dll (placeholders), winmm.dll (waveOut on SDL3 audio), xinput1_4.dll (SDL3 gamepads),
 * gdi32.dll (no-op blits). Any other name is dlopen()ed. GetModuleHandle(NULL) is the main program. */
HMODULE LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags);
HMODULE LoadLibraryW(LPCWSTR name);
HMODULE LoadLibraryA(LPCSTR name);
HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags);
HMODULE GetModuleHandleW(LPCWSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *module);
FARPROC GetProcAddress(HMODULE module, LPCSTR name);
BOOL FreeLibrary(HMODULE module);

/* ------------------------------------------------------------------------------------------------ windows on SDL3 */
typedef LRESULT (*WNDPROC)(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
typedef struct { UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon;
 HCURSOR hCursor; HBRUSH hbrBackground; LPCWSTR lpszMenuName, lpszClassName; } WNDCLASSW;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG, *LPMSG;
typedef struct { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore, fIncUpdate; BYTE rgbReserved[32]; } PAINTSTRUCT;
typedef struct { UINT length, flags, showCmd; POINT ptMinPosition, ptMaxPosition; RECT rcNormalPosition; } WINDOWPLACEMENT;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO, *LPMONITORINFO;
ATOM RegisterClassW(const WNDCLASSW *window_class);
HWND CreateWindowExW(DWORD ex_style, LPCWSTR class_name, LPCWSTR title, DWORD style, int x, int y, int width, int height,
                     HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter);
BOOL DestroyWindow(HWND window);
BOOL ShowWindow(HWND window, int command);
BOOL UpdateWindow(HWND window);
BOOL SetWindowTextW(HWND window, LPCWSTR text);
BOOL SetWindowPos(HWND window, HWND after, int x, int y, int cx, int cy, UINT flags);
BOOL GetClientRect(HWND window, LPRECT rect);
BOOL GetWindowRect(HWND window, LPRECT rect);
BOOL AdjustWindowRect(LPRECT rect, DWORD style, BOOL menu);
BOOL IsIconic(HWND window);
BOOL IsZoomed(HWND window);
BOOL IsWindowVisible(HWND window);
LONG_PTR GetWindowLongPtrW(HWND window, int index);
LONG_PTR SetWindowLongPtrW(HWND window, int index, LONG_PTR value);
BOOL GetWindowPlacement(HWND window, WINDOWPLACEMENT *placement);
BOOL SetWindowPlacement(HWND window, const WINDOWPLACEMENT *placement);
HMONITOR MonitorFromWindow(HWND window, DWORD flags);
BOOL GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info);
BOOL PeekMessageW(LPMSG message, HWND window, UINT first, UINT last, UINT remove);
BOOL TranslateMessage(const MSG *message);
LRESULT DispatchMessageW(const MSG *message);
LRESULT DefWindowProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
HDC BeginPaint(HWND window, PAINTSTRUCT *paint);
BOOL EndPaint(HWND window, const PAINTSTRUCT *paint);
BOOL InvalidateRect(HWND window, const RECT *rect, BOOL erase);
HCURSOR LoadCursorW(HINSTANCE instance, LPCWSTR name);
HCURSOR SetCursor(HCURSOR cursor);
BOOL GetCursorPos(LPPOINT point);
BOOL SetCursorPos(int x, int y);
BOOL ClipCursor(const RECT *rect);
HWND SetCapture(HWND window);
BOOL ReleaseCapture(void);
HWND GetCapture(void);
HWND GetForegroundWindow(void);
int MapWindowPoints(HWND from, HWND to, LPPOINT points, UINT count);
BOOL ScreenToClient(HWND window, LPPOINT point);
SHORT GetKeyState(int key);
SHORT GetAsyncKeyState(int key);
UINT MapVirtualKeyW(UINT code, UINT type);
int MessageBoxW(HWND window, LPCWSTR text, LPCWSTR caption, UINT type);
int MessageBoxA(HWND window, LPCSTR text, LPCSTR caption, UINT type);
#define MB_OK 0u
#define MB_ICONERROR 0x10u
#define MB_ICONWARNING 0x30u
#define MB_ICONINFORMATION 0x40u
#define MAPVK_VK_TO_VSC 0u
#define MAPVK_VSC_TO_VK 1u
#define MAPVK_VK_TO_CHAR 2u
#define MAPVK_VSC_TO_VK_EX 3u

#define CW_USEDEFAULT ((int)0x80000000)
#define WS_OVERLAPPED 0x00000000u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_CAPTION 0x00C00000u
#define WS_SYSMENU 0x00080000u
#define WS_THICKFRAME 0x00040000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
#define HWND_TOP ((HWND)0)
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST ((HWND)(intptr_t)-2)
#define SWP_NOSIZE 0x1u
#define SWP_NOMOVE 0x2u
#define SWP_NOZORDER 0x4u
#define SWP_NOACTIVATE 0x10u
#define SWP_FRAMECHANGED 0x20u
#define SWP_SHOWWINDOW 0x40u
#define SWP_NOOWNERZORDER 0x200u
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_RESTORE 9
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_MAXIMIZE 3
#define MONITOR_DEFAULTTONULL 0u
#define MONITOR_DEFAULTTOPRIMARY 1u
#define MONITOR_DEFAULTTONEAREST 2u
#define COLOR_WINDOW 5
#define PM_NOREMOVE 0u
#define PM_REMOVE 1u
#define IDC_ARROW MAKEINTRESOURCEW(32512)

#define WM_NULL 0x0000u
#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_PAINT 0x000Fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_SETCURSOR 0x0020u
#define WM_NCLBUTTONDOWN 0x00A1u
#define WM_INPUT 0x00FFu
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCHAR 0x0106u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP 0x0208u
#define WM_MOUSEWHEEL 0x020Au
#define WM_XBUTTONDOWN 0x020Bu
#define WM_XBUTTONUP 0x020Cu
#define WM_MOUSEHWHEEL 0x020Eu
#define WM_CAPTURECHANGED 0x0215u
#define WM_EXITSIZEMOVE 0x0232u
#define WM_USER 0x0400u
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define HTCLIENT 1
#define HTCAPTION 2
#define MK_LBUTTON 0x0001u
#define MK_RBUTTON 0x0002u
#define MK_SHIFT 0x0004u
#define MK_CONTROL 0x0008u
#define MK_MBUTTON 0x0010u
#define MK_XBUTTON1 0x0020u
#define MK_XBUTTON2 0x0040u
#define XBUTTON1 0x0001u
#define XBUTTON2 0x0002u
#define WHEEL_DELTA 120
#define GET_WHEEL_DELTA_WPARAM(w) ((short)HIWORD(w))
#define GET_X_LPARAM(l) ((int)(short)LOWORD(l))
#define GET_Y_LPARAM(l) ((int)(short)HIWORD(l))

/* Raw mouse input: the game's relative movement while it holds the mouse (SDL relative mouse mode). */
typedef struct { USHORT usUsagePage, usUsage; DWORD dwFlags; HWND hwndTarget; } RAWINPUTDEVICE, *PRAWINPUTDEVICE;
typedef struct { DWORD dwType, dwSize; HANDLE hDevice; WPARAM wParam; } RAWINPUTHEADER;
typedef struct { USHORT usFlags; union { ULONG ulButtons; struct { USHORT usButtonFlags, usButtonData; }; };
 ULONG ulRawButtons; LONG lLastX, lLastY; ULONG ulExtraInformation; } RAWMOUSE;
typedef struct { RAWINPUTHEADER header; union { RAWMOUSE mouse; BYTE raw_[24]; } data; } RAWINPUT, *PRAWINPUT;
#define RIM_TYPEMOUSE 0u
#define RIM_TYPEKEYBOARD 1u
#define RID_INPUT 0x10000003u
#define RID_HEADER 0x10000005u
#define RIDEV_REMOVE 1u
#define RIDEV_INPUTSINK 0x100u
#define MOUSE_MOVE_RELATIVE 0u
#define MOUSE_MOVE_ABSOLUTE 1u
BOOL RegisterRawInputDevices(const RAWINPUTDEVICE *devices, UINT count, UINT size);
UINT GetRawInputData(HRAWINPUT input, UINT command, LPVOID data, UINT *size, UINT header_size);

/* Virtual-key codes (the launcher's Controls store these). */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F24 0x87
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_102 0xE2

/* GDI: the software display path (before the Vulkan backend) is not used on Linux; these do nothing. */
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage;
 LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO;
#pragma pack(push, 2)
typedef struct { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; } BITMAPFILEHEADER;
#pragma pack(pop)
#define BI_RGB 0u
#define DIB_RGB_COLORS 0u
#define SRCCOPY 0x00CC0020u
#define GDI_ERROR (-1)
#define TRANSPARENT 1
#define OPAQUE 2
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define FW_NORMAL 400
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define CLEARTYPE_QUALITY 5
#define ANTIALIASED_QUALITY 4
#define NONANTIALIASED_QUALITY 3
#define OUT_TT_PRECIS 4
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_CHARSET 1
#define ANSI_CHARSET 0
#define DEFAULT_PITCH 0
#define VARIABLE_PITCH 2
#define FW_NORMAL 400
#define FW_BOLD 700
#define FW_HEAVY 900
#define FW_BLACK 900
#define TRANSPARENT 1
#define OPAQUE 2
#define DEFAULT_PITCH 0
#define FF_DONTCARE 0
HDC GetDC(HWND window);
int ReleaseDC(HWND window, HDC dc);
HDC CreateCompatibleDC(HDC dc);
BOOL DeleteDC(HDC dc);
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *info, UINT usage, void **bits, HANDLE section, DWORD offset);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ object);
BOOL DeleteObject(HGDIOBJ object);
HFONT CreateFontW(int height, int width, int escapement, int orientation, int weight, DWORD italic, DWORD underline,
                  DWORD strike, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality, DWORD pitch,
                  LPCWSTR face);
COLORREF SetTextColor(HDC dc, COLORREF color);
int SetBkMode(HDC dc, int mode);
BOOL TextOutW(HDC dc, int x, int y, LPCWSTR text, int count);
BOOL GetTextExtentPoint32W(HDC dc, LPCWSTR text, int count, LPSIZE size);
BOOL GdiFlush(void);

/* PE headers: the program's own image is not a PE file here (nvn_hle.h's sampling profiler is Windows-only). */
typedef struct { WORD e_magic; WORD e_unused_[29]; LONG e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { BYTE Name[8]; union { DWORD PhysicalAddress, VirtualSize; } Misc; DWORD VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations_,
 PointerToLinenumbers_; WORD NumberOfRelocations_, NumberOfLinenumbers_; DWORD Characteristics; } IMAGE_SECTION_HEADER;
typedef struct { WORD Machine, NumberOfSections; DWORD TimeDateStamp_, PointerToSymbolTable_, NumberOfSymbols_;
 WORD SizeOfOptionalHeader, Characteristics; } IMAGE_FILE_HEADER;
typedef struct { DWORD Signature; IMAGE_FILE_HEADER FileHeader; BYTE OptionalHeader[240]; } IMAGE_NT_HEADERS64;
#define IMAGE_FIRST_SECTION(nt) ((IMAGE_SECTION_HEADER *)((const BYTE *)&(nt)->OptionalHeader + (nt)->FileHeader.SizeOfOptionalHeader))
#define IMAGE_SCN_MEM_EXECUTE 0x20000000u

/* Linux-only extras for the patched Vulkan backend (device.c) */
struct VkInstance_T;
const char *const *win32_posix_vulkan_instance_extensions(uint32_t *count);
int win32_posix_vulkan_surface(HWND window, struct VkInstance_T *instance, uint64_t *surface);
/* The SDL window behind an HWND (NULL if none). */
void *win32_posix_sdl_window(HWND window);
