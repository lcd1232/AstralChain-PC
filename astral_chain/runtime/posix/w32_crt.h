/* Linux host: the Microsoft C runtime names the runtime uses, for files with and without <windows.h>. */
#pragma once
#include <stdio.h>
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define stricmp strcasecmp
#define _wcsicmp wcscasecmp
#define _snprintf snprintf
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _fileno fileno
#define _strdup strdup
#define _access access
#define _unlink unlink
#define _getpid getpid
#define sprintf_s snprintf
__attribute__((noreturn)) void _exit(int status);   /* (mingw declares it in <stdlib.h>; no <unistd.h> here) */
