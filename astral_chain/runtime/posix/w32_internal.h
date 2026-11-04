/* Linux host: shared internals of the Win32 subset (w32_*.c). Not included by the runtime. */
#pragma once
#include <windows.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

enum { W32_EVENT = 1, W32_THREAD, W32_PROCESS, W32_FILE, W32_FIND, W32_MAPPING };

/* One kernel object. HANDLEs point at a W32Ref (DuplicateHandle makes another W32Ref to the same object), so a
 * handle is closed once, as on Windows, and the object lives until its last handle is closed. */
typedef struct W32Waiter W32Waiter;
typedef struct W32Object {
 int type;
 atomic_int refs;
 /* waitable state (under w32_sync_lock) */
 int signaled, manual_reset;
 W32Waiter *waiters;
 /* type data */
 union {
  struct { pthread_t thread; DWORD id; DWORD exit_code; int started, suspended_start; LPTHREAD_START_ROUTINE start;
           void *parameter; pthread_mutex_t gate_lock; pthread_cond_t gate; int released;
           atomic_int suspend_count; volatile int frozen; CONTEXT context; uintptr_t stack_low, stack_high; } thread;
  struct { pid_t pid; DWORD exit_code; } process;
  struct { int fd; int delete_on_close; char *path; int is_pipe; } file;
  struct { void *dir; char *directory; char *pattern; int dot_state; } find;
  struct { int fd; uint64_t size; DWORD protect; } mapping;
 } u;
} W32Object;

typedef struct { uint32_t magic; W32Object *object; } W32Ref;
#define W32_REF_MAGIC 0x57334852u

HANDLE w32_handle_new(W32Object *object);
W32Object *w32_object(HANDLE handle, int type);   /* NULL (and ERROR_INVALID_HANDLE) if not that type */
W32Object *w32_object_new(int type);
void w32_object_release(W32Object *object);

extern pthread_mutex_t w32_sync_lock;
void w32_signal_locked(W32Object *object);        /* object->signaled=1 and wake its waiters */

DWORD w32_error_from_errno(int error);
void w32_set_errno_error(void);

/* Paths: UTF-16 code units (one per wchar_t) or ANSI (UTF-8) to a host path ("\\?\" removed, '\' to '/'). */
char *w32_path_from_wide(LPCWSTR path);
char *w32_path_from_ansi(LPCSTR path);
char *w32_utf8_from_wide(LPCWSTR text, int count);
int w32_wide_from_utf8(const char *text, LPWSTR out, int capacity);   /* code units written incl. NUL, 0 if too long */

/* Thread-local current thread object (NULL on threads not made by CreateThread before their first use). */
W32Object *w32_current_thread_object(void);
void w32_thread_signal_setup(void);    /* per thread: alternate signal stack (memory faults run the VEH chain there) */

/* SDL side (w32_sdl.c) */
int w32_sdl_ensure(unsigned flags);
int w32_sdl_message_wait(DWORD milliseconds);      /* window thread: wait for input; 1 if some arrived */
FARPROC w32_builtin_proc(const char *module, const char *name);
int w32_builtin_module(const char *name);
