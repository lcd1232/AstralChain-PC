/* Linux host: <direct.h>. (No <unistd.h> here: its pipe() would clash with backend variables of that name.) */
#pragma once
#include <stddef.h>
#include <sys/stat.h>
#include "w32_crt.h"
int rmdir(const char *path);
char *getcwd(char *buffer, size_t size);
int chdir(const char *path);
#define _mkdir(path) mkdir((path), 0777)
#define _rmdir rmdir
#define _getcwd getcwd
#define _chdir chdir
