/* Linux host: Win32 files and directories on POSIX file descriptors. Paths arrive as UTF-16 units or UTF-8 with
 * backslashes; "\\?\" is dropped. A file opened without sharing takes an exclusive flock(), so a second open (by this
 * or another process) fails with a sharing violation, as the save lock expects. */
#include "w32_internal.h"
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

HANDLE w32_file_from_fd(int fd, int is_pipe) {
 W32Object *object = w32_object_new(W32_FILE);
 if (!object) { close(fd); return NULL; }
 object->u.file.fd = fd;
 object->u.file.is_pipe = is_pipe;
 HANDLE handle = w32_handle_new(object);
 if (!handle) { close(fd); free(object); }
 return handle;
}

static int handle_fd(HANDLE handle) {
 W32Object *object = w32_object(handle, W32_FILE);
 return object ? object->u.file.fd : -1;
}

HANDLE GetStdHandle(DWORD which) {
 static HANDLE standard[3];
 int index = which == STD_INPUT_HANDLE ? 0 : which == STD_OUTPUT_HANDLE ? 1 : which == STD_ERROR_HANDLE ? 2 : -1;
 if (index < 0) return INVALID_HANDLE_VALUE;
 if (!standard[index]) {
  W32Object *object = w32_object_new(W32_FILE);
  object->u.file.fd = index;
  atomic_store(&object->refs, 2);   /* never closed */
  standard[index] = w32_handle_new(object);
 }
 return standard[index];
}

static HANDLE create_file(char *path, DWORD access, DWORD share, DWORD disposition, DWORD flags) {
 if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
 int mode = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) ?
            ((access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) ? O_RDWR : O_WRONLY) : O_RDONLY;
 int oflags = O_CLOEXEC;
 switch (disposition) {
 case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
 case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
 case OPEN_EXISTING: break;
 case OPEN_ALWAYS: oflags |= O_CREAT; break;
 case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
 default: free(path); SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
 }
 struct stat st;
 int existed = stat(path, &st) == 0;
 if (existed && S_ISDIR(st.st_mode)) {
  if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) { free(path); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
  mode = O_RDONLY;
  oflags = O_CLOEXEC | O_DIRECTORY;
 }
 if ((oflags & O_TRUNC) && mode == O_RDONLY) mode = O_RDWR;
 if (!existed && (oflags & O_CREAT) && mode == O_RDONLY) mode = O_RDWR;
 int fd = open(path, mode | oflags, 0644);
 if (fd < 0) {
  DWORD error = errno == EEXIST ? ERROR_FILE_EXISTS : w32_error_from_errno(errno);
  if (errno == ENOENT) {
   char *slash = strrchr(path, '/');
   if (slash && slash != path) {
    *slash = 0;
    if (stat(path, &st)) error = ERROR_PATH_NOT_FOUND;
   }
  }
  free(path);
  SetLastError(error);
  return INVALID_HANDLE_VALUE;
 }
 if (!share && !(oflags & O_DIRECTORY) && flock(fd, LOCK_EX | LOCK_NB)) {
  close(fd);
  free(path);
  SetLastError(ERROR_SHARING_VIOLATION);
  return INVALID_HANDLE_VALUE;
 }
 W32Object *object = w32_object_new(W32_FILE);
 object->u.file.fd = fd;
 object->u.file.path = path;
 object->u.file.delete_on_close = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
 HANDLE handle = w32_handle_new(object);
 /* Windows reports an opened existing file for OPEN_ALWAYS / CREATE_ALWAYS */
 SetLastError(existed && (disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
 return handle;
}

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
                   DWORD flags, HANDLE template_file) {
 (void)attributes; (void)template_file;
 return create_file(w32_path_from_wide(path), access, share, disposition, flags);
}
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES attributes, DWORD disposition,
                   DWORD flags, HANDLE template_file) {
 (void)attributes; (void)template_file;
 return create_file(w32_path_from_ansi(path), access, share, disposition, flags);
}

BOOL ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read_count, LPOVERLAPPED overlapped) {
 int fd = handle_fd(file);
 if (fd < 0) return FALSE;
 size_t done = 0;
 while (done < size) {
  ssize_t n = overlapped ? pread(fd, (char *)buffer + done, size - done,
                                 (off_t)(((uint64_t)overlapped->OffsetHigh << 32 | overlapped->Offset) + done))
                         : read(fd, (char *)buffer + done, size - done);
  if (n < 0) {
   if (errno == EINTR) continue;
   if (read_count) *read_count = (DWORD)done;
   if (errno == EFAULT) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
   w32_set_errno_error();
   return FALSE;
  }
  if (!n) break;
  done += (size_t)n;
  W32Object *object = w32_object(file, W32_FILE);
  if (object && object->u.file.is_pipe) break;   /* a pipe read returns what is there */
 }
 if (read_count) *read_count = (DWORD)done;
 W32Object *object = w32_object(file, W32_FILE);
 if (!done && size && object && object->u.file.is_pipe) { SetLastError(ERROR_BROKEN_PIPE); return FALSE; }
 return TRUE;
}

BOOL WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
 int fd = handle_fd(file);
 if (fd < 0) return FALSE;
 size_t done = 0;
 while (done < size) {
  ssize_t n = overlapped ? pwrite(fd, (const char *)buffer + done, size - done,
                                  (off_t)(((uint64_t)overlapped->OffsetHigh << 32 | overlapped->Offset) + done))
                         : write(fd, (const char *)buffer + done, size - done);
  if (n < 0) {
   if (errno == EINTR) continue;
   if (written) *written = (DWORD)done;
   if (errno == EFAULT) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
   w32_set_errno_error();
   return FALSE;
  }
  done += (size_t)n;
 }
 if (written) *written = (DWORD)done;
 return TRUE;
}

BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method) {
 int fd = handle_fd(file);
 if (fd < 0) return FALSE;
 off_t at = lseek(fd, (off_t)distance.QuadPart, method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
 if (at < 0) { w32_set_errno_error(); return FALSE; }
 if (position) position->QuadPart = at;
 return TRUE;
}
DWORD SetFilePointer(HANDLE file, LONG distance, PLONG high, DWORD method) {
 LARGE_INTEGER d, p;
 d.QuadPart = high ? ((LONGLONG)*high << 32) | (DWORD)distance : distance;
 if (!SetFilePointerEx(file, d, &p, method)) return INVALID_SET_FILE_POINTER;
 if (high) *high = p.HighPart;
 return p.LowPart;
}
BOOL GetFileSizeEx(HANDLE file, PLARGE_INTEGER size) {
 int fd = handle_fd(file);
 struct stat st;
 if (fd < 0) return FALSE;
 if (fstat(fd, &st)) { w32_set_errno_error(); return FALSE; }
 size->QuadPart = st.st_size;
 return TRUE;
}
DWORD GetFileSize(HANDLE file, LPDWORD high) {
 LARGE_INTEGER size;
 if (!GetFileSizeEx(file, &size)) return INVALID_FILE_SIZE;
 if (high) *high = (DWORD)size.HighPart;
 return size.LowPart;
}
BOOL SetEndOfFile(HANDLE file) {
 int fd = handle_fd(file);
 if (fd < 0) return FALSE;
 off_t at = lseek(fd, 0, SEEK_CUR);
 if (at < 0 || ftruncate(fd, at)) { w32_set_errno_error(); return FALSE; }
 return TRUE;
}
BOOL FlushFileBuffers(HANDLE file) {
 int fd = handle_fd(file);
 if (fd < 0) return FALSE;
 if (fsync(fd) && errno != EINVAL && errno != EROFS) { w32_set_errno_error(); return FALSE; }
 return TRUE;
}

static void filetime_of(const struct timespec *t, FILETIME *out) {
 uint64_t v = 116444736000000000ull + (uint64_t)t->tv_sec * 10000000ull + (uint64_t)t->tv_nsec / 100;
 out->dwLowDateTime = (DWORD)v;
 out->dwHighDateTime = (DWORD)(v >> 32);
}
static DWORD attributes_of(const struct stat *st, const char *name) {
 DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
 if (!(st->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
 const char *leaf = name ? strrchr(name, '/') : NULL;
 leaf = leaf ? leaf + 1 : name;
 if (leaf && leaf[0] == '.' && strcmp(leaf, ".") && strcmp(leaf, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
 return a;
}

BOOL GetFileInformationByHandle(HANDLE file, LPBY_HANDLE_FILE_INFORMATION information) {
 W32Object *object = w32_object(file, W32_FILE);
 struct stat st;
 if (!object) return FALSE;
 if (fstat(object->u.file.fd, &st)) { w32_set_errno_error(); return FALSE; }
 memset(information, 0, sizeof(*information));
 information->dwFileAttributes = attributes_of(&st, object->u.file.path);
 filetime_of(&st.st_ctim, &information->ftCreationTime);
 filetime_of(&st.st_atim, &information->ftLastAccessTime);
 filetime_of(&st.st_mtim, &information->ftLastWriteTime);
 information->dwVolumeSerialNumber = (DWORD)st.st_dev;
 information->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
 information->nFileSizeLow = (DWORD)st.st_size;
 information->nNumberOfLinks = (DWORD)st.st_nlink;
 information->nFileIndexHigh = (DWORD)((uint64_t)st.st_ino >> 32);
 information->nFileIndexLow = (DWORD)st.st_ino;
 return TRUE;
}

static BOOL result_of(int status, char *a, char *b) {
 DWORD error = status ? w32_error_from_errno(errno) : 0;
 free(a); free(b);
 if (status) { SetLastError(error); return FALSE; }
 return TRUE;
}

BOOL DeleteFileW(LPCWSTR path) { char *p = w32_path_from_wide(path); return result_of(p ? unlink(p) : -1, p, NULL); }
BOOL DeleteFileA(LPCSTR path) { char *p = w32_path_from_ansi(path); return result_of(p ? unlink(p) : -1, p, NULL); }

static int copy_file(const char *from, const char *to, int fail_if_exists) {
 int in = open(from, O_RDONLY | O_CLOEXEC);
 if (in < 0) return -1;
 int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (fail_if_exists ? O_EXCL : 0), 0644);
 if (out < 0) { int e = errno; close(in); errno = e; return -1; }
 char buffer[1 << 16];
 ssize_t n;
 int status = 0;
 while ((n = read(in, buffer, sizeof buffer)) > 0) {
  for (ssize_t done = 0; done < n;) {
   ssize_t w = write(out, buffer + done, (size_t)(n - done));
   if (w < 0) { if (errno == EINTR) continue; status = -1; break; }
   done += w;
  }
  if (status) break;
 }
 if (n < 0) status = -1;
 int e = errno;
 close(in);
 if (close(out)) status = -1;
 errno = e;
 return status;
}

static int move_file(const char *from, const char *to, DWORD flags) {
 struct stat st;
 if (!(flags & MOVEFILE_REPLACE_EXISTING) && !lstat(to, &st)) { errno = EEXIST; return -1; }
 if (!rename(from, to)) return 0;
 if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED)) {
  if (copy_file(from, to, 0)) return -1;
  return unlink(from);
 }
 return -1;
}
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
 char *a = w32_path_from_wide(from), *b = w32_path_from_wide(to);
 return result_of(a && b ? move_file(a, b, flags) : -1, a, b);
}
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
 char *a = w32_path_from_ansi(from), *b = w32_path_from_ansi(to);
 return result_of(a && b ? move_file(a, b, flags) : -1, a, b);
}
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists) {
 char *a = w32_path_from_wide(from), *b = w32_path_from_wide(to);
 return result_of(a && b ? copy_file(a, b, fail_if_exists) : -1, a, b);
}
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
 char *a = w32_path_from_ansi(from), *b = w32_path_from_ansi(to);
 return result_of(a && b ? copy_file(a, b, fail_if_exists) : -1, a, b);
}

BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES attributes) {
 (void)attributes;
 char *p = w32_path_from_wide(path);
 return result_of(p ? mkdir(p, 0755) : -1, p, NULL);
}
BOOL CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES attributes) {
 (void)attributes;
 char *p = w32_path_from_ansi(path);
 return result_of(p ? mkdir(p, 0755) : -1, p, NULL);
}
BOOL RemoveDirectoryW(LPCWSTR path) { char *p = w32_path_from_wide(path); return result_of(p ? rmdir(p) : -1, p, NULL); }
BOOL RemoveDirectoryA(LPCSTR path) { char *p = w32_path_from_ansi(path); return result_of(p ? rmdir(p) : -1, p, NULL); }

static DWORD file_attributes(char *path) {
 struct stat st;
 if (!path || stat(path, &st)) {
  SetLastError(path ? w32_error_from_errno(errno) : ERROR_INVALID_PARAMETER);
  free(path);
  return INVALID_FILE_ATTRIBUTES;
 }
 DWORD a = attributes_of(&st, path);
 free(path);
 return a;
}
DWORD GetFileAttributesW(LPCWSTR path) { return file_attributes(w32_path_from_wide(path)); }
DWORD GetFileAttributesA(LPCSTR path) { return file_attributes(w32_path_from_ansi(path)); }

static BOOL disk_free(char *path, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes) {
 struct statvfs s;
 if (!path || statvfs(path, &s)) return result_of(-1, path, NULL);
 free(path);
 if (available) available->QuadPart = (ULONGLONG)s.f_bavail * s.f_frsize;
 if (total) total->QuadPart = (ULONGLONG)s.f_blocks * s.f_frsize;
 if (free_bytes) free_bytes->QuadPart = (ULONGLONG)s.f_bfree * s.f_frsize;
 return TRUE;
}
BOOL GetDiskFreeSpaceExW(LPCWSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes) {
 return disk_free(path ? w32_path_from_wide(path) : strdup("."), available, total, free_bytes);
}
BOOL GetDiskFreeSpaceExA(LPCSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free_bytes) {
 return disk_free(path ? w32_path_from_ansi(path) : strdup("."), available, total, free_bytes);
}

DWORD GetTempPathA(DWORD size, LPSTR buffer) {
 const char *dir = getenv("TMPDIR");
 if (!dir || !*dir) dir = "/tmp";
 size_t n = strlen(dir);
 int slash = dir[n - 1] != '/';
 if (size <= n + (size_t)slash) return (DWORD)(n + (size_t)slash + 1);
 memcpy(buffer, dir, n);
 if (slash) buffer[n++] = '/';
 buffer[n] = 0;
 return (DWORD)n;
}
DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer) {
 char path[4096];
 if (!getcwd(path, sizeof path)) { w32_set_errno_error(); return 0; }
 size_t n = strlen(path);
 if (size <= n) return (DWORD)n + 1;
 memcpy(buffer, path, n + 1);
 return (DWORD)n;
}
DWORD GetFullPathNameA(LPCSTR path, DWORD size, LPSTR buffer, LPSTR *file_part) {
 char *p = w32_path_from_ansi(path);
 if (!p) return 0;
 char full[4096];
 if (p[0] == '/') snprintf(full, sizeof full, "%s", p);
 else {
  char cwd[2048];
  if (!getcwd(cwd, sizeof cwd)) { free(p); return 0; }
  snprintf(full, sizeof full, "%s/%s", cwd, p);
 }
 free(p);
 size_t n = strlen(full);
 if (size <= n) return (DWORD)n + 1;
 memcpy(buffer, full, n + 1);
 if (file_part) { char *slash = strrchr(buffer, '/'); *file_part = slash ? slash + 1 : buffer; }
 return (DWORD)n;
}

/* ------------------------------------------------------------------------------------------------ find */
/* "dir\pattern": the pattern is matched case-insensitively ("*.*" is "*"); "." and ".." are listed, as on Windows. */
static HANDLE find_first(char *pattern, char *name_out, size_t name_size, struct stat *st_out) {
 if (!pattern) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
 char *slash = strrchr(pattern, '/');
 char *directory = slash ? strndup(pattern, (size_t)(slash - pattern)) : strdup(".");
 char *leaf = strdup(slash ? slash + 1 : pattern);
 free(pattern);
 if (!directory[0]) { free(directory); directory = strdup("/"); }
 if (!strcmp(leaf, "*.*")) leaf[1] = 0;
 DIR *dir = opendir(directory);
 if (!dir) {
  DWORD error = errno == ENOENT ? ERROR_PATH_NOT_FOUND : w32_error_from_errno(errno);
  free(directory); free(leaf);
  SetLastError(error);
  return INVALID_HANDLE_VALUE;
 }
 W32Object *object = w32_object_new(W32_FIND);
 object->u.find.dir = dir;
 object->u.find.directory = directory;
 object->u.find.pattern = leaf;
 HANDLE handle = w32_handle_new(object);
 extern BOOL w32_find_next(HANDLE, char *, size_t, struct stat *);
 if (!w32_find_next(handle, name_out, name_size, st_out)) {
  CloseHandle(handle);
  SetLastError(ERROR_FILE_NOT_FOUND);
  return INVALID_HANDLE_VALUE;
 }
 return handle;
}

BOOL w32_find_next(HANDLE find, char *name_out, size_t name_size, struct stat *st_out) {
 W32Object *object = w32_object(find, W32_FIND);
 if (!object) return FALSE;
 for (;;) {
  struct dirent *entry = readdir(object->u.find.dir);
  if (!entry) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
  if (fnmatch(object->u.find.pattern, entry->d_name, FNM_CASEFOLD | FNM_PERIOD * 0)) continue;
  char full[4096];
  snprintf(full, sizeof full, "%s/%s", object->u.find.directory, entry->d_name);
  if (stat(full, st_out)) continue;
  snprintf(name_out, name_size, "%s", entry->d_name);
  return TRUE;
 }
}

void w32_find_close_object(W32Object *object) {
 if (object->u.find.dir) closedir(object->u.find.dir);
 free(object->u.find.directory);
 free(object->u.find.pattern);
 object->u.find.dir = NULL;
 object->u.find.directory = object->u.find.pattern = NULL;
}

static void fill_find_common(const struct stat *st, const char *name, DWORD *attributes, FILETIME *c, FILETIME *a,
                             FILETIME *w, DWORD *high, DWORD *low) {
 *attributes = attributes_of(st, name);
 filetime_of(&st->st_ctim, c);
 filetime_of(&st->st_atim, a);
 filetime_of(&st->st_mtim, w);
 *high = S_ISDIR(st->st_mode) ? 0 : (DWORD)((uint64_t)st->st_size >> 32);
 *low = S_ISDIR(st->st_mode) ? 0 : (DWORD)st->st_size;
}
static void fill_find_w(LPWIN32_FIND_DATAW data, const struct stat *st, const char *name) {
 memset(data, 0, sizeof(*data));
 fill_find_common(st, name, &data->dwFileAttributes, &data->ftCreationTime, &data->ftLastAccessTime,
                  &data->ftLastWriteTime, &data->nFileSizeHigh, &data->nFileSizeLow);
 if (!w32_wide_from_utf8(name, data->cFileName, MAX_PATH)) data->cFileName[0] = 0;
}
static void fill_find_a(LPWIN32_FIND_DATAA data, const struct stat *st, const char *name) {
 memset(data, 0, sizeof(*data));
 fill_find_common(st, name, &data->dwFileAttributes, &data->ftCreationTime, &data->ftLastAccessTime,
                  &data->ftLastWriteTime, &data->nFileSizeHigh, &data->nFileSizeLow);
 snprintf(data->cFileName, MAX_PATH, "%s", name);
}

HANDLE FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
 char name[1024]; struct stat st;
 HANDLE h = find_first(w32_path_from_wide(pattern), name, sizeof name, &st);
 if (h != INVALID_HANDLE_VALUE) fill_find_w(data, &st, name);
 return h;
}
BOOL FindNextFileW(HANDLE find, LPWIN32_FIND_DATAW data) {
 char name[1024]; struct stat st;
 if (!w32_find_next(find, name, sizeof name, &st)) return FALSE;
 fill_find_w(data, &st, name);
 return TRUE;
}
HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data) {
 char name[1024]; struct stat st;
 HANDLE h = find_first(w32_path_from_ansi(pattern), name, sizeof name, &st);
 if (h != INVALID_HANDLE_VALUE) fill_find_a(data, &st, name);
 return h;
}
BOOL FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data) {
 char name[1024]; struct stat st;
 if (!w32_find_next(find, name, sizeof name, &st)) return FALSE;
 fill_find_a(data, &st, name);
 return TRUE;
}
BOOL FindClose(HANDLE find) {
 W32Object *object = w32_object(find, W32_FIND);
 if (!object) return FALSE;
 return CloseHandle(find);
}

/* ------------------------------------------------------------------------------------------------ C runtime wide */
FILE *_wfopen(const wchar_t *path, const wchar_t *mode) {
 char *p = w32_path_from_wide(path), *m = w32_utf8_from_wide(mode, -1);
 FILE *f = p && m ? fopen(p, m) : NULL;
 free(p); free(m);
 return f;
}
int _wmkdir(const wchar_t *path) { char *p = w32_path_from_wide(path); int r = p ? mkdir(p, 0755) : -1; free(p); return r; }
int _wremove(const wchar_t *path) { char *p = w32_path_from_wide(path); int r = p ? remove(p) : -1; free(p); return r; }

/* ------------------------------------------------------------------------------------------------ module paths */
DWORD GetModuleFileNameA(HMODULE module, LPSTR buffer, DWORD size) {
 char path[4096];
 ssize_t n;
 if (!module) {
  n = readlink("/proc/self/exe", path, sizeof path - 1);
  if (n < 0) { w32_set_errno_error(); return 0; }
  path[n] = 0;
 } else {
  Dl_info info;
  if (!dladdr(module, &info) || !info.dli_fname) { SetLastError(ERROR_MOD_NOT_FOUND); return 0; }
  snprintf(path, sizeof path, "%s", info.dli_fname);
  n = (ssize_t)strlen(path);
 }
 if (!size) return 0;
 size_t copy = (size_t)n < size - 1 ? (size_t)n : size - 1;
 memcpy(buffer, path, copy);
 buffer[copy] = 0;
 if ((size_t)n >= size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
 return (DWORD)n;
}
DWORD GetModuleFileNameW(HMODULE module, LPWSTR buffer, DWORD size) {
 char path[4096];
 DWORD n = GetModuleFileNameA(module, path, sizeof path);
 if (!n) return 0;
 int units = MultiByteToWideChar(CP_UTF8, 0, path, -1, buffer, (int)size);
 return units ? (DWORD)units - 1 : size;
}
