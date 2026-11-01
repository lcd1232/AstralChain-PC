/* Linux host: no minidumps (LoadLibraryA("dbghelp.dll") fails; use the system's core dumps). Types only. */
#pragma once
#include <windows.h>
typedef enum { MiniDumpNormal = 0, MiniDumpWithDataSegs = 1, MiniDumpWithFullMemory = 2, MiniDumpWithHandleData = 4,
 MiniDumpWithUnloadedModules = 0x20, MiniDumpWithThreadInfo = 0x1000 } MINIDUMP_TYPE;
typedef struct { DWORD ThreadId; PEXCEPTION_POINTERS ExceptionPointers; BOOL ClientPointers; }
 MINIDUMP_EXCEPTION_INFORMATION, *PMINIDUMP_EXCEPTION_INFORMATION;
typedef struct { ULONG UserStreamCount; void *UserStreamArray; } MINIDUMP_USER_STREAM_INFORMATION, *PMINIDUMP_USER_STREAM_INFORMATION;
typedef struct { void *CallbackRoutine; PVOID CallbackParam; } MINIDUMP_CALLBACK_INFORMATION, *PMINIDUMP_CALLBACK_INFORMATION;
