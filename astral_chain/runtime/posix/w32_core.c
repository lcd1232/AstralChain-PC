/* Linux host: Win32 core subset on POSIX. Errors, handles, events and waits, threads, processes and pipes,
 * environment, time, strings, heap, BCrypt (getrandom and SHA-256). */
#include "w32_internal.h"
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <unistd.h>
#include <bcrypt.h>

extern char **environ;

/* ------------------------------------------------------------------------------------------------ errors */
static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD error) { last_error = error; }

DWORD w32_error_from_errno(int error) {
 switch (error) {
 case 0: return ERROR_SUCCESS;
 case ENOENT: return ERROR_FILE_NOT_FOUND;
 case ENOTDIR: return ERROR_PATH_NOT_FOUND;
 case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
 case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
 case EBADF: return ERROR_INVALID_HANDLE;
 case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
 case EEXIST: return ERROR_ALREADY_EXISTS;
 case ENOSPC: case EDQUOT: return ERROR_DISK_FULL;
 case EINVAL: return ERROR_INVALID_PARAMETER;
 case ENAMETOOLONG: return ERROR_INVALID_NAME;
 case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
 case EPIPE: return ERROR_BROKEN_PIPE;
 case EWOULDBLOCK: return ERROR_SHARING_VIOLATION;
 case EXDEV: return ERROR_NOT_SUPPORTED;
 case ETIMEDOUT: return ERROR_TIMEOUT;
 case EIO: return ERROR_READ_FAULT;
 case EISDIR: return ERROR_ACCESS_DENIED;
 default: return ERROR_INVALID_FUNCTION;
 }
}
void w32_set_errno_error(void) { last_error = w32_error_from_errno(errno); }

/* ------------------------------------------------------------------------------------------------ handles */
pthread_mutex_t w32_sync_lock = PTHREAD_MUTEX_INITIALIZER;

W32Object *w32_object_new(int type) {
 W32Object *object = calloc(1, sizeof(*object));
 if (!object) { last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
 object->type = type;
 atomic_store(&object->refs, 1);
 return object;
}

HANDLE w32_handle_new(W32Object *object) {
 W32Ref *ref = malloc(sizeof(*ref));
 if (!ref) { last_error = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
 ref->magic = W32_REF_MAGIC;
 ref->object = object;
 return ref;
}

static W32Ref *ref_of(HANDLE handle) {
 if (!handle || handle == INVALID_HANDLE_VALUE || (uintptr_t)handle < 4096) return NULL;
 W32Ref *ref = handle;
 return ref->magic == W32_REF_MAGIC ? ref : NULL;
}

W32Object *w32_object(HANDLE handle, int type) {
 if (handle == (HANDLE)(intptr_t)-2) {  /* GetCurrentThread() pseudo handle */
  W32Object *self = w32_current_thread_object();
  if (self && (!type || type == W32_THREAD)) return self;
 }
 W32Ref *ref = ref_of(handle);
 if (!ref || (type && ref->object->type != type)) { last_error = ERROR_INVALID_HANDLE; return NULL; }
 return ref->object;
}

void w32_find_close_object(W32Object *object);   /* w32_files.c */
static void object_destroy(W32Object *object) {
 switch (object->type) {
 case W32_FILE:
  if (object->u.file.fd >= 0) close(object->u.file.fd);
  if (object->u.file.delete_on_close && object->u.file.path) unlink(object->u.file.path);
  free(object->u.file.path);
  break;
 case W32_MAPPING:
  if (object->u.mapping.fd >= 0) close(object->u.mapping.fd);
  break;
 case W32_FIND:
  w32_find_close_object(object);
  break;
 case W32_THREAD:
  /* a thread object of a running thread stays referenced by the thread itself */
  pthread_detach(object->u.thread.thread);
  break;
 default: break;
 }
 free(object);
}

void w32_object_release(W32Object *object) {
 if (object && atomic_fetch_sub(&object->refs, 1) == 1) object_destroy(object);
}

BOOL CloseHandle(HANDLE handle) {
 W32Ref *ref = ref_of(handle);
 if (!ref) {
  if (handle == (HANDLE)(intptr_t)-2 || handle == (HANDLE)(intptr_t)-1) return TRUE;
  last_error = ERROR_INVALID_HANDLE;
  return FALSE;
 }
 W32Object *object = ref->object;
 ref->magic = 0;
 free(ref);
 w32_object_release(object);
 return TRUE;
}

BOOL DuplicateHandle(HANDLE source_process, HANDLE source, HANDLE target_process, LPHANDLE target, DWORD access,
                     BOOL inherit, DWORD options) {
 (void)source_process; (void)target_process; (void)access; (void)inherit;
 W32Object *object = w32_object(source, 0);
 if (!object) return FALSE;
 atomic_fetch_add(&object->refs, 1);
 HANDLE copy = w32_handle_new(object);
 if (!copy) { w32_object_release(object); return FALSE; }
 *target = copy;
 if (options & DUPLICATE_CLOSE_SOURCE) CloseHandle(source);
 return TRUE;
}

BOOL SetHandleInformation(HANDLE handle, DWORD mask, DWORD flags) {
 (void)mask; (void)flags;
 return w32_object(handle, 0) != NULL;
}

/* ------------------------------------------------------------------------------------------------ events, waits */
struct W32Waiter { pthread_cond_t cond; };
typedef struct W32WaitNode { W32Waiter *waiter; struct W32WaitNode *next; } W32WaitNode;

void w32_signal_locked(W32Object *object) {
 object->signaled = 1;
 for (W32WaitNode *node = (W32WaitNode *)object->waiters; node; node = node->next) pthread_cond_signal(&node->waiter->cond);
}

static HANDLE event_create(BOOL manual_reset, BOOL initial) {
 W32Object *object = w32_object_new(W32_EVENT);
 if (!object) return NULL;
 object->manual_reset = manual_reset != 0;
 object->signaled = initial != 0;
 HANDLE handle = w32_handle_new(object);
 if (!handle) free(object);
 return handle;
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, LPCWSTR name) {
 (void)attributes; (void)name;
 return event_create(manual_reset, initial);
}
HANDLE CreateEventA(LPSECURITY_ATTRIBUTES attributes, BOOL manual_reset, BOOL initial, LPCSTR name) {
 (void)attributes; (void)name;
 return event_create(manual_reset, initial);
}
BOOL SetEvent(HANDLE event) {
 W32Object *object = w32_object(event, W32_EVENT);
 if (!object) return FALSE;
 pthread_mutex_lock(&w32_sync_lock);
 w32_signal_locked(object);
 pthread_mutex_unlock(&w32_sync_lock);
 return TRUE;
}
BOOL ResetEvent(HANDLE event) {
 W32Object *object = w32_object(event, W32_EVENT);
 if (!object) return FALSE;
 pthread_mutex_lock(&w32_sync_lock);
 object->signaled = 0;
 pthread_mutex_unlock(&w32_sync_lock);
 return TRUE;
}

static int waitable(W32Object *object) {
 return object && (object->type == W32_EVENT || object->type == W32_THREAD || object->type == W32_PROCESS);
}

static void consume(W32Object *object) {
 if (object->type == W32_EVENT && !object->manual_reset) object->signaled = 0;
}

static struct timespec deadline_after(DWORD milliseconds) {
 struct timespec t;
 clock_gettime(CLOCK_MONOTONIC, &t);
 t.tv_sec += milliseconds / 1000;
 t.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
 if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
 return t;
}

static DWORD wait_objects(DWORD count, const HANDLE *handles, BOOL all, DWORD milliseconds) {
 if (!count || count > MAXIMUM_WAIT_OBJECTS) { last_error = ERROR_INVALID_PARAMETER; return WAIT_FAILED; }
 W32Object *objects[MAXIMUM_WAIT_OBJECTS];
 for (DWORD i = 0; i < count; i++) {
  objects[i] = w32_object(handles[i], 0);
  if (!waitable(objects[i])) { last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
 }
 W32Waiter waiter;
 pthread_condattr_t attr;
 pthread_condattr_init(&attr);
 pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
 pthread_cond_init(&waiter.cond, &attr);
 pthread_condattr_destroy(&attr);
 W32WaitNode nodes[MAXIMUM_WAIT_OBJECTS];
 struct timespec deadline = deadline_after(milliseconds == INFINITE ? 0 : milliseconds);
 DWORD result = WAIT_TIMEOUT;
 pthread_mutex_lock(&w32_sync_lock);
 for (;;) {
  if (all) {
   DWORD ready = 0;
   for (DWORD i = 0; i < count; i++) ready += objects[i]->signaled != 0;
   if (ready == count) {
    for (DWORD i = 0; i < count; i++) consume(objects[i]);
    result = WAIT_OBJECT_0;
    break;
   }
  } else {
   DWORD i = 0;
   while (i < count && !objects[i]->signaled) i++;
   if (i < count) { consume(objects[i]); result = WAIT_OBJECT_0 + i; break; }
  }
  if (!milliseconds) break;
  for (DWORD i = 0; i < count; i++) {
   nodes[i].waiter = &waiter;
   nodes[i].next = (W32WaitNode *)objects[i]->waiters;
   objects[i]->waiters = (W32Waiter *)&nodes[i];
  }
  int timed_out = 0;
  if (milliseconds == INFINITE) pthread_cond_wait(&waiter.cond, &w32_sync_lock);
  else timed_out = pthread_cond_timedwait(&waiter.cond, &w32_sync_lock, &deadline) == ETIMEDOUT;
  for (DWORD i = 0; i < count; i++) {
   W32WaitNode **link = (W32WaitNode **)&objects[i]->waiters;
   while (*link && *link != &nodes[i]) link = &(*link)->next;
   if (*link) *link = nodes[i].next;
  }
  if (timed_out) {
   /* one last look: a signal may have raced the timeout */
   milliseconds = 0;
  }
 }
 pthread_mutex_unlock(&w32_sync_lock);
 pthread_cond_destroy(&waiter.cond);
 return result;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds) { return wait_objects(1, &handle, FALSE, milliseconds); }
DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable) {
 (void)alertable;
 return wait_objects(1, &handle, FALSE, milliseconds);
}
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD milliseconds) {
 return wait_objects(count, handles, all, milliseconds);
}

/* Window message waits: objects first, then input in slices (the window thread's SDL queue). */
DWORD MsgWaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, DWORD milliseconds, DWORD wake_mask, DWORD flags) {
 (void)wake_mask;
 ULONGLONG start = GetTickCount64();
 for (;;) {
  if (count) {
   DWORD r = wait_objects(count, handles, (flags & MWMO_WAITALL) != 0, 0);
   if (r != WAIT_TIMEOUT) return r;
  }
  ULONGLONG elapsed = GetTickCount64() - start;
  if (milliseconds != INFINITE && elapsed >= milliseconds) return WAIT_TIMEOUT;
  DWORD slice = milliseconds == INFINITE ? 4 : (DWORD)(milliseconds - elapsed < 4 ? milliseconds - elapsed : 4);
  if (!count) slice = milliseconds == INFINITE ? 100 : (DWORD)(milliseconds - elapsed);
  if (w32_sdl_message_wait(slice)) return WAIT_OBJECT_0 + count;
 }
}
DWORD MsgWaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD milliseconds, DWORD wake_mask) {
 return MsgWaitForMultipleObjectsEx(count, handles, milliseconds, wake_mask, all ? MWMO_WAITALL : 0);
}

BOOL SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD milliseconds, ULONG flags) {
 (void)flags;
 if (milliseconds == INFINITE) { pthread_cond_wait(&cv->cond, &lock->mutex); return TRUE; }
 /* on the monotonic clock (glibc 2.30): a wall-clock step (NTP, resume) must not stretch a 20 ms wait */
 struct timespec t = deadline_after(milliseconds);
 if (pthread_cond_clockwait(&cv->cond, &lock->mutex, CLOCK_MONOTONIC, &t) == ETIMEDOUT) { last_error = ERROR_TIMEOUT; return FALSE; }
 return TRUE;
}

void InitializeCriticalSection(LPCRITICAL_SECTION section) {
 pthread_mutexattr_t attr;
 pthread_mutexattr_init(&attr);
 pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
 pthread_mutex_init(&section->mutex, &attr);
 pthread_mutexattr_destroy(&attr);
}

/* ------------------------------------------------------------------------------------------------ threads */
static __thread W32Object *current_thread;
static int suspend_signal_installed;
#define SUSPEND_SIGNAL (SIGRTMIN + 3)

static void context_from_ucontext(CONTEXT *context, const ucontext_t *u) {
 const greg_t *g = u->uc_mcontext.gregs;
 context->Rax = (DWORD64)g[REG_RAX]; context->Rcx = (DWORD64)g[REG_RCX]; context->Rdx = (DWORD64)g[REG_RDX];
 context->Rbx = (DWORD64)g[REG_RBX]; context->Rsp = (DWORD64)g[REG_RSP]; context->Rbp = (DWORD64)g[REG_RBP];
 context->Rsi = (DWORD64)g[REG_RSI]; context->Rdi = (DWORD64)g[REG_RDI]; context->R8 = (DWORD64)g[REG_R8];
 context->R9 = (DWORD64)g[REG_R9]; context->R10 = (DWORD64)g[REG_R10]; context->R11 = (DWORD64)g[REG_R11];
 context->R12 = (DWORD64)g[REG_R12]; context->R13 = (DWORD64)g[REG_R13]; context->R14 = (DWORD64)g[REG_R14];
 context->R15 = (DWORD64)g[REG_R15]; context->Rip = (DWORD64)g[REG_RIP];
}

static void suspend_handler(int sig, siginfo_t *info, void *uc) {
 (void)sig; (void)info;
 W32Object *self = current_thread;
 if (!self) return;
 int saved = errno;
 context_from_ucontext(&self->u.thread.context, uc);
 __atomic_store_n(&self->u.thread.frozen, 1, __ATOMIC_RELEASE);
 while (atomic_load(&self->u.thread.suspend_count) > 0) {
  struct timespec t = {0, 200000};
  nanosleep(&t, NULL);
 }
 __atomic_store_n(&self->u.thread.frozen, 0, __ATOMIC_RELEASE);
 errno = saved;
}

static void stack_limits(W32Object *object) {
 pthread_attr_t attr;
 void *address; size_t size;
 if (!pthread_getattr_np(pthread_self(), &attr)) {
  if (!pthread_attr_getstack(&attr, &address, &size)) {
   object->u.thread.stack_low = (uintptr_t)address;
   object->u.thread.stack_high = (uintptr_t)address + size;
  }
  pthread_attr_destroy(&attr);
 }
}

W32Object *w32_current_thread_object(void) {
 if (!current_thread) {
  W32Object *object = w32_object_new(W32_THREAD);
  if (!object) return NULL;
  object->u.thread.thread = pthread_self();
  object->u.thread.id = (DWORD)syscall(SYS_gettid);
  object->u.thread.started = 1;
  object->u.thread.exit_code = STILL_ACTIVE;
  atomic_fetch_add(&object->refs, 1);   /* never released: the thread keeps it (threads not made by CreateThread) */
  stack_limits(object);
  current_thread = object;
 }
 return current_thread;
}

static void *thread_main(void *argument) {
 W32Object *object = argument;
 current_thread = object;
 object->u.thread.id = (DWORD)syscall(SYS_gettid);
 stack_limits(object);
 w32_thread_signal_setup();
 pthread_mutex_lock(&object->u.thread.gate_lock);
 object->u.thread.started = 1;
 pthread_cond_broadcast(&object->u.thread.gate);
 while (!object->u.thread.released) pthread_cond_wait(&object->u.thread.gate, &object->u.thread.gate_lock);
 pthread_mutex_unlock(&object->u.thread.gate_lock);
 DWORD code = object->u.thread.start(object->u.thread.parameter);
 ExitThread(code);
}

static void thread_finish(W32Object *object, DWORD code) {
 pthread_mutex_lock(&w32_sync_lock);
 object->u.thread.exit_code = code;
 w32_signal_locked(object);
 pthread_mutex_unlock(&w32_sync_lock);
}

static void thread_exit_cleanup(void *argument) { (void)argument; }

__attribute__((noreturn)) void ExitThread(DWORD code) {
 W32Object *self = current_thread;
 if (self) {
  thread_finish(self, code);
  current_thread = NULL;
  w32_object_release(self);   /* the thread's own reference */
 }
 pthread_cleanup_push(thread_exit_cleanup, NULL);
 pthread_exit(NULL);
 pthread_cleanup_pop(0);
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack_size, LPTHREAD_START_ROUTINE start, LPVOID parameter,
                    DWORD flags, LPDWORD thread_id) {
 (void)attributes;
 if (!suspend_signal_installed) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = suspend_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigfillset(&sa.sa_mask);
  sigaction(SUSPEND_SIGNAL, &sa, NULL);
  suspend_signal_installed = 1;
 }
 W32Object *object = w32_object_new(W32_THREAD);
 if (!object) return NULL;
 object->u.thread.start = start;
 object->u.thread.parameter = parameter;
 object->u.thread.exit_code = STILL_ACTIVE;
 object->u.thread.released = !(flags & CREATE_SUSPENDED);
 object->u.thread.suspended_start = (flags & CREATE_SUSPENDED) != 0;
 pthread_mutex_init(&object->u.thread.gate_lock, NULL);
 pthread_cond_init(&object->u.thread.gate, NULL);
 atomic_store(&object->refs, 2);   /* the handle and the running thread */
 HANDLE handle = w32_handle_new(object);
 if (!handle) { free(object); return NULL; }
 pthread_attr_t attr;
 pthread_attr_init(&attr);
 size_t size = stack_size ? stack_size : 8u << 20;
 if (size < (1u << 20)) size = 1u << 20;
 size = (size + 65535) & ~(size_t)65535;
 pthread_attr_setstacksize(&attr, size);
 int error = pthread_create(&object->u.thread.thread, &attr, thread_main, object);
 pthread_attr_destroy(&attr);
 if (error) {
  free(handle); free(object);
  last_error = w32_error_from_errno(error);
  return NULL;
 }
 /* the id is known once the thread runs */
 pthread_mutex_lock(&object->u.thread.gate_lock);
 while (!object->u.thread.started) pthread_cond_wait(&object->u.thread.gate, &object->u.thread.gate_lock);
 pthread_mutex_unlock(&object->u.thread.gate_lock);
 if (thread_id) *thread_id = object->u.thread.id;
 return handle;
}

DWORD ResumeThread(HANDLE thread) {
 W32Object *object = w32_object(thread, W32_THREAD);
 if (!object) return (DWORD)-1;
 if (object->u.thread.suspended_start) {
  pthread_mutex_lock(&object->u.thread.gate_lock);
  object->u.thread.suspended_start = 0;
  object->u.thread.released = 1;
  pthread_cond_broadcast(&object->u.thread.gate);
  pthread_mutex_unlock(&object->u.thread.gate_lock);
  return 1;
 }
 int previous = atomic_load(&object->u.thread.suspend_count);
 if (previous > 0 && atomic_fetch_sub(&object->u.thread.suspend_count, 1) == 1)
  for (int i = 0; i < 100000 && __atomic_load_n(&object->u.thread.frozen, __ATOMIC_ACQUIRE); i++) sched_yield();
 return (DWORD)previous;
}

/* Suspension (the sampling profiler): a signal parks the thread in its handler, which records its registers. */
DWORD SuspendThread(HANDLE thread) {
 W32Object *object = w32_object(thread, W32_THREAD);
 if (!object || !suspend_signal_installed) { last_error = ERROR_INVALID_HANDLE; return (DWORD)-1; }
 if (object == current_thread) { last_error = ERROR_INVALID_PARAMETER; return (DWORD)-1; }
 int previous = atomic_fetch_add(&object->u.thread.suspend_count, 1);
 if (previous == 0) {
  if (pthread_kill(object->u.thread.thread, SUSPEND_SIGNAL)) {
   atomic_fetch_sub(&object->u.thread.suspend_count, 1);
   last_error = ERROR_INVALID_HANDLE;
   return (DWORD)-1;
  }
  for (int i = 0; i < 100000 && !__atomic_load_n(&object->u.thread.frozen, __ATOMIC_ACQUIRE); i++) sched_yield();
 }
 return (DWORD)previous;
}

BOOL GetThreadContext(HANDLE thread, PCONTEXT context) {
 W32Object *object = w32_object(thread, W32_THREAD);
 if (!object || !__atomic_load_n(&object->u.thread.frozen, __ATOMIC_ACQUIRE)) { last_error = ERROR_INVALID_HANDLE; return FALSE; }
 DWORD flags = context->ContextFlags;
 *context = object->u.thread.context;
 context->ContextFlags = flags;
 return TRUE;
}

BOOL GetExitCodeThread(HANDLE thread, LPDWORD code) {
 W32Object *object = w32_object(thread, W32_THREAD);
 if (!object) return FALSE;
 pthread_mutex_lock(&w32_sync_lock);
 *code = object->u.thread.exit_code;
 pthread_mutex_unlock(&w32_sync_lock);
 return TRUE;
}

BOOL SetThreadPriority(HANDLE thread, int priority) {
 (void)priority;
 return w32_object(thread, W32_THREAD) != NULL;
}
int GetThreadPriority(HANDLE thread) { (void)thread; return THREAD_PRIORITY_NORMAL; }
DWORD GetThreadId(HANDLE thread) {
 W32Object *object = w32_object(thread, W32_THREAD);
 return object ? object->u.thread.id : 0;
}
HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
DWORD GetCurrentThreadId(void) {
 static __thread DWORD id;
 if (!id) id = (DWORD)syscall(SYS_gettid);
 return id;
}
HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
void GetCurrentThreadStackLimits(PULONG_PTR low, PULONG_PTR high) {
 W32Object *self = w32_current_thread_object();
 *low = self ? self->u.thread.stack_low : 0;
 *high = self ? self->u.thread.stack_high : 0;
}
DWORD_PTR SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask) { (void)thread; return mask ? 1 : 0; }

void Sleep(DWORD milliseconds) {
 if (!milliseconds) { sched_yield(); return; }
 struct timespec t = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L};
 while (nanosleep(&t, &t) && errno == EINTR) {}
}
DWORD SleepEx(DWORD milliseconds, BOOL alertable) { (void)alertable; Sleep(milliseconds); return 0; }
BOOL SwitchToThread(void) { sched_yield(); return TRUE; }
DWORD GetActiveProcessorCount(WORD group) { (void)group; long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (DWORD)n : 1; }

void GetSystemInfo(LPSYSTEM_INFO info) {
 memset(info, 0, sizeof(*info));
 info->wProcessorArchitecture = 9;   /* PROCESSOR_ARCHITECTURE_AMD64 */
 info->dwPageSize = 4096;
 info->dwAllocationGranularity = 65536;
 info->lpMinimumApplicationAddress = (LPVOID)(uintptr_t)0x10000;
 info->lpMaximumApplicationAddress = (LPVOID)(uintptr_t)0x7ffffffeffffull;
 info->dwNumberOfProcessors = GetActiveProcessorCount(0);
 info->dwActiveProcessorMask = info->dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << info->dwNumberOfProcessors) - 1);
}
void GetNativeSystemInfo(LPSYSTEM_INFO info) { GetSystemInfo(info); }

/* ------------------------------------------------------------------------------------------------ processes */
__attribute__((noreturn)) void ExitProcess(UINT code) {
 fflush(stdout); fflush(stderr);
 exit((int)code);
}

BOOL TerminateProcess(HANDLE process, UINT code) {
 if (process == GetCurrentProcess()) { fflush(stdout); fflush(stderr); _exit((int)code); }
 W32Object *object = w32_object(process, W32_PROCESS);
 if (!object) return FALSE;
 return kill(object->u.process.pid, SIGKILL) == 0;
}

static void *reaper(void *argument) {
 W32Object *object = argument;
 int status = 0;
 while (waitpid(object->u.process.pid, &status, 0) < 0 && errno == EINTR) {}
 pthread_mutex_lock(&w32_sync_lock);
 object->u.process.exit_code = WIFEXITED(status) ? (DWORD)WEXITSTATUS(status) : 0xC0000000u | (DWORD)WTERMSIG(status);
 w32_signal_locked(object);
 pthread_mutex_unlock(&w32_sync_lock);
 w32_object_release(object);
 return NULL;
}

/* Command line to argv, as the Microsoft C runtime splits it (quotes; backslashes before a quote). */
static char **split_command_line(const char *line) {
 size_t length = strlen(line);
 char **argv = calloc(length / 2 + 2, sizeof(char *));
 char *buffer = malloc(length + 1);
 if (!argv || !buffer) { free(argv); free(buffer); return NULL; }
 size_t argc = 0;
 const char *p = line;
 char *out = buffer;
 for (;;) {
  while (*p == ' ' || *p == '\t') p++;
  if (!*p) break;
  argv[argc++] = out;
  int quoted = 0;
  while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
   size_t slashes = 0;
   while (*p == '\\') { slashes++; p++; }
   if (*p == '"') {
    for (size_t i = 0; i < slashes / 2; i++) *out++ = '\\';
    if (slashes % 2) { *out++ = '"'; p++; }
    else {
     if (quoted && p[1] == '"') { *out++ = '"'; p++; }
     else quoted = !quoted;
     p++;
    }
   } else {
    for (size_t i = 0; i < slashes; i++) *out++ = '\\';
    if (*p && (quoted || (*p != ' ' && *p != '\t'))) *out++ = *p++;
   }
  }
  *out++ = 0;
 }
 argv[argc] = NULL;
 if (!argc) { free(buffer); free(argv); return NULL; }
 return argv;
}

static int file_fd(HANDLE handle) {
 W32Object *object = w32_object(handle, W32_FILE);
 return object ? object->u.file.fd : -1;
}

BOOL CreateProcessA(LPCSTR application, LPSTR command_line, LPSECURITY_ATTRIBUTES process_attributes,
                    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment,
                    LPCSTR directory, LPSTARTUPINFOA startup, LPPROCESS_INFORMATION information) {
 (void)process_attributes; (void)thread_attributes; (void)inherit; (void)flags; (void)environment;
 char **argv = split_command_line(command_line ? command_line : application ? application : "");
 if (!argv) { last_error = ERROR_INVALID_PARAMETER; return FALSE; }
 for (char *c = argv[0]; *c; c++) if (*c == '\\') *c = '/';
 const char *program = application ? application : argv[0];
 posix_spawn_file_actions_t actions;
 posix_spawn_file_actions_init(&actions);
 if (startup && (startup->dwFlags & STARTF_USESTDHANDLES)) {
  HANDLE standard[3] = {startup->hStdInput, startup->hStdOutput, startup->hStdError};
  for (int i = 0; i < 3; i++) {
   int fd = file_fd(standard[i]);
   if (fd >= 0) posix_spawn_file_actions_adddup2(&actions, fd, i);
  }
 }
 if (directory) posix_spawn_file_actions_addchdir_np(&actions, directory);
 posix_spawnattr_t attr;
 posix_spawnattr_init(&attr);
 sigset_t defaults;
 sigemptyset(&defaults);
 sigaddset(&defaults, SIGPIPE);   /* (ignored in this process; the child gets the default action back) */
 posix_spawnattr_setsigdefault(&attr, &defaults);
 posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
 pid_t pid;
 int error = strchr(program, '/') ? posix_spawn(&pid, program, &actions, &attr, argv, environ)
                                  : posix_spawnp(&pid, program, &actions, &attr, argv, environ);
 posix_spawnattr_destroy(&attr);
 posix_spawn_file_actions_destroy(&actions);
 free(argv[0]); free(argv);
 if (error) { last_error = w32_error_from_errno(error); return FALSE; }
 W32Object *object = w32_object_new(W32_PROCESS);
 W32Object *thread = w32_object_new(W32_EVENT);
 object->u.process.pid = pid;
 object->u.process.exit_code = STILL_ACTIVE;
 atomic_store(&object->refs, 2);   /* the handle and the reaper */
 pthread_t t;
 pthread_attr_t thread_attr;
 pthread_attr_init(&thread_attr);
 pthread_attr_setstacksize(&thread_attr, 256 * 1024);
 pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
 pthread_create(&t, &thread_attr, reaper, object);
 pthread_attr_destroy(&thread_attr);
 information->hProcess = w32_handle_new(object);
 information->hThread = w32_handle_new(thread);
 information->dwProcessId = (DWORD)pid;
 information->dwThreadId = (DWORD)pid;
 return TRUE;
}

BOOL GetExitCodeProcess(HANDLE process, LPDWORD code) {
 if (process == GetCurrentProcess()) { *code = STILL_ACTIVE; return TRUE; }
 W32Object *object = w32_object(process, W32_PROCESS);
 if (!object) return FALSE;
 pthread_mutex_lock(&w32_sync_lock);
 *code = object->u.process.exit_code;
 pthread_mutex_unlock(&w32_sync_lock);
 return TRUE;
}

HANDLE w32_file_from_fd(int fd, int is_pipe);   /* w32_files.c */

BOOL CreatePipe(PHANDLE read_end, PHANDLE write_end, LPSECURITY_ATTRIBUTES attributes, DWORD size) {
 (void)attributes;
 int fds[2];
 if (pipe2(fds, O_CLOEXEC)) { w32_set_errno_error(); return FALSE; }
 if (size > 65536) fcntl(fds[1], F_SETPIPE_SZ, (int)size);
 *read_end = w32_file_from_fd(fds[0], 1);
 *write_end = w32_file_from_fd(fds[1], 1);
 return *read_end && *write_end;
}

BOOL PeekNamedPipe(HANDLE pipe, LPVOID buffer, DWORD size, LPDWORD read, LPDWORD available, LPDWORD left) {
 int fd = file_fd(pipe);
 if (fd < 0) return FALSE;
 int n = 0;
 if (ioctl(fd, FIONREAD, &n)) { w32_set_errno_error(); return FALSE; }
 if (buffer && size) {
  if (read) *read = 0;   /* (only the available count is needed: no peeking into pipes) */
 } else if (read) *read = 0;
 if (available) *available = (DWORD)n;
 if (left) *left = 0;
 return TRUE;
}

/* ------------------------------------------------------------------------------------------------ environment */
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size) {
 const char *value = getenv(name);
 if (!value) { last_error = ERROR_ENVVAR_NOT_FOUND; return 0; }
 size_t length = strlen(value);
 if (buffer && size > length) { memcpy(buffer, value, length + 1); return (DWORD)length; }
 return (DWORD)length + 1;
}

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size) {
 char *key = w32_utf8_from_wide(name, -1);
 const char *value = key ? getenv(key) : NULL;
 free(key);
 if (!value) { last_error = ERROR_ENVVAR_NOT_FOUND; return 0; }
 int needed = MultiByteToWideChar(CP_UTF8, 0, value, -1, NULL, 0);
 if (needed <= 0) return 0;
 if (buffer && size >= (DWORD)needed) {
  MultiByteToWideChar(CP_UTF8, 0, value, -1, buffer, (int)size);
  return (DWORD)needed - 1;
 }
 return (DWORD)needed;
}

BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value) {
 return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

/* ------------------------------------------------------------------------------------------------ time */
/* The performance counter ticks at 10 MHz, as on current Windows (the runtime multiplies counts by 10^9). */
BOOL QueryPerformanceCounter(LARGE_INTEGER *counter) {
 struct timespec t;
 clock_gettime(CLOCK_MONOTONIC, &t);
 counter->QuadPart = (LONGLONG)t.tv_sec * 10000000LL + t.tv_nsec / 100;
 return TRUE;
}
BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency) { frequency->QuadPart = 10000000LL; return TRUE; }
ULONGLONG GetTickCount64(void) {
 struct timespec t;
 clock_gettime(CLOCK_MONOTONIC, &t);
 return (ULONGLONG)t.tv_sec * 1000ull + (ULONGLONG)t.tv_nsec / 1000000ull;
}
DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

#define FILETIME_UNIX_EPOCH 116444736000000000ull
static void filetime_from(uint64_t value, LPFILETIME time) {
 time->dwLowDateTime = (DWORD)value;
 time->dwHighDateTime = (DWORD)(value >> 32);
}
void GetSystemTimePreciseAsFileTime(LPFILETIME time) {
 struct timespec t;
 clock_gettime(CLOCK_REALTIME, &t);
 filetime_from(FILETIME_UNIX_EPOCH + (uint64_t)t.tv_sec * 10000000ull + (uint64_t)t.tv_nsec / 100, time);
}
void GetSystemTimeAsFileTime(LPFILETIME time) { GetSystemTimePreciseAsFileTime(time); }

static void systemtime_from(const struct tm *tm, long nanoseconds, LPSYSTEMTIME time) {
 time->wYear = (WORD)(tm->tm_year + 1900); time->wMonth = (WORD)(tm->tm_mon + 1); time->wDayOfWeek = (WORD)tm->tm_wday;
 time->wDay = (WORD)tm->tm_mday; time->wHour = (WORD)tm->tm_hour; time->wMinute = (WORD)tm->tm_min;
 time->wSecond = (WORD)tm->tm_sec; time->wMilliseconds = (WORD)(nanoseconds / 1000000);
}
void GetLocalTime(LPSYSTEMTIME time) {
 struct timespec t; struct tm tm;
 clock_gettime(CLOCK_REALTIME, &t);
 localtime_r(&t.tv_sec, &tm);
 systemtime_from(&tm, t.tv_nsec, time);
}
void GetSystemTime(LPSYSTEMTIME time) {
 struct timespec t; struct tm tm;
 clock_gettime(CLOCK_REALTIME, &t);
 gmtime_r(&t.tv_sec, &tm);
 systemtime_from(&tm, t.tv_nsec, time);
}
DWORD GetTimeZoneInformation(LPTIME_ZONE_INFORMATION information) {
 memset(information, 0, sizeof(*information));
 time_t now = time(NULL);
 struct tm tm;
 localtime_r(&now, &tm);
 tzset();
 information->Bias = (LONG)(-tm.tm_gmtoff / 60);
 for (int i = 0; i < 31 && tzname[0] && tzname[0][i]; i++) information->StandardName[i] = (WCHAR)tzname[0][i];
 for (int i = 0; i < 31 && tzname[1] && tzname[1][i]; i++) information->DaylightName[i] = (WCHAR)tzname[1][i];
 return TIME_ZONE_ID_UNKNOWN;
}

/* ------------------------------------------------------------------------------------------------ strings */
/* Wide strings hold UTF-16 code units, one per wchar_t. */
int MultiByteToWideChar(UINT code_page, DWORD flags, LPCSTR input, int input_size, LPWSTR output, int output_size) {
 (void)code_page;
 const unsigned char *s = (const unsigned char *)input;
 size_t n = input_size < 0 ? strlen(input) + 1 : (size_t)input_size;
 int written = 0;
 for (size_t i = 0; i < n;) {
  uint32_t c = s[i];
  int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
  if (extra < 0 || i + (size_t)extra >= n + (extra ? 0 : 1)) {
   if (flags & MB_ERR_INVALID_CHARS) { last_error = ERROR_NO_UNICODE_TRANSLATION; return 0; }
   c = 0xFFFD; extra = 0;
  } else {
   if (extra) c &= 0x3Fu >> extra;
   for (int k = 1; k <= extra; k++) {
    if ((s[i + k] & 0xC0) != 0x80) {
     if (flags & MB_ERR_INVALID_CHARS) { last_error = ERROR_NO_UNICODE_TRANSLATION; return 0; }
     c = 0xFFFD; extra = k - 1; break;
    }
    c = (c << 6) | (s[i + k] & 0x3F);
   }
  }
  i += (size_t)extra + 1;
  int units = c >= 0x10000 ? 2 : 1;
  if (output_size) {
   if (written + units > output_size) { last_error = ERROR_INSUFFICIENT_BUFFER; return 0; }
   if (units == 2) {
    c -= 0x10000;
    output[written] = (WCHAR)(0xD800 + (c >> 10));
    output[written + 1] = (WCHAR)(0xDC00 + (c & 0x3FF));
   } else output[written] = (WCHAR)c;
  }
  written += units;
 }
 return written;
}

int WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR input, int input_size, LPSTR output, int output_size,
                        LPCSTR default_char, BOOL *used_default) {
 (void)code_page; (void)default_char;
 if (used_default) *used_default = FALSE;
 size_t n = 0;
 if (input_size < 0) { while (input[n]) n++; n++; } else n = (size_t)input_size;
 int written = 0;
 for (size_t i = 0; i < n; i++) {
  uint32_t c = (uint32_t)input[i] & 0xFFFFu;
  if ((uint32_t)input[i] > 0xFFFF) c = (uint32_t)input[i];   /* (a UTF-32 literal) */
  else if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && ((uint32_t)input[i + 1] & 0xFC00) == 0xDC00) {
   c = 0x10000 + ((c - 0xD800) << 10) + (((uint32_t)input[i + 1]) & 0x3FF);
   i++;
  } else if (c >= 0xD800 && c < 0xE000) {
   if (flags & WC_ERR_INVALID_CHARS) { last_error = ERROR_NO_UNICODE_TRANSLATION; return 0; }
   c = 0xFFFD;
  }
  char b[4]; int k;
  if (c < 0x80) { b[0] = (char)c; k = 1; }
  else if (c < 0x800) { b[0] = (char)(0xC0 | (c >> 6)); b[1] = (char)(0x80 | (c & 0x3F)); k = 2; }
  else if (c < 0x10000) { b[0] = (char)(0xE0 | (c >> 12)); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); k = 3; }
  else { b[0] = (char)(0xF0 | (c >> 18)); b[1] = (char)(0x80 | ((c >> 12) & 0x3F)); b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); k = 4; }
  if (output_size) {
   if (written + k > output_size) { last_error = ERROR_INSUFFICIENT_BUFFER; return 0; }
   memcpy(output + written, b, (size_t)k);
  }
  written += k;
 }
 return written;
}

char *w32_utf8_from_wide(LPCWSTR text, int count) {
 if (!text) return NULL;
 int n = WideCharToMultiByte(CP_UTF8, 0, text, count, NULL, 0, NULL, NULL);
 char *out = malloc((size_t)n + 1);
 if (!out) return NULL;
 WideCharToMultiByte(CP_UTF8, 0, text, count, out, n, NULL, NULL);
 out[n] = 0;
 return out;
}

int w32_wide_from_utf8(const char *text, LPWSTR out, int capacity) {
 int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
 if (n <= 0 || n > capacity) return 0;
 return MultiByteToWideChar(CP_UTF8, 0, text, -1, out, capacity);
}

/* "\\?\" is dropped; a drive letter ("Z:\home\...", as the save code requires one) stands for the root. */
static char *normalize_path(char *path) {
 if (!path) return NULL;
 char *p = path;
 if (!strncmp(p, "\\\\?\\", 4) || !strncmp(p, "//?/", 4)) memmove(p, p + 4, strlen(p + 4) + 1);
 if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/'))
  memmove(p, p + 2, strlen(p + 2) + 1);
 for (char *c = path; *c; c++) if (*c == '\\') *c = '/';
 return path;
}
char *w32_path_from_wide(LPCWSTR path) { return normalize_path(w32_utf8_from_wide(path, -1)); }
char *w32_path_from_ansi(LPCSTR path) { return path ? normalize_path(strdup(path)) : NULL; }

/* ------------------------------------------------------------------------------------------------ heap */
HANDLE GetProcessHeap(void) { return (HANDLE)(intptr_t)0x48454150; }
LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size) {
 (void)heap;
 return (flags & HEAP_ZERO_MEMORY) ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
}
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID memory) { (void)heap; (void)flags; free(memory); return TRUE; }

/* ------------------------------------------------------------------------------------------------ BCrypt */
typedef struct { uint32_t state[8]; uint64_t length; uint8_t block[64]; size_t used; } Sha256;
static const uint32_t sha256_k[64] = {
 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(Sha256 *s, const uint8_t *p) {
 uint32_t w[64];
 for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
 for (int i = 16; i < 64; i++) {
  uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
  uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
  w[i] = w[i - 16] + s0 + w[i - 7] + s1;
 }
 uint32_t a = s->state[0], b = s->state[1], c = s->state[2], d = s->state[3], e = s->state[4], f = s->state[5],
          g = s->state[6], h = s->state[7];
 for (int i = 0; i < 64; i++) {
  uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
  uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
  h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
 }
 s->state[0] += a; s->state[1] += b; s->state[2] += c; s->state[3] += d;
 s->state[4] += e; s->state[5] += f; s->state[6] += g; s->state[7] += h;
}
static void sha256(const uint8_t *data, size_t size, uint8_t out[32]) {
 Sha256 s = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
 size_t i = 0;
 for (; i + 64 <= size; i += 64) sha256_block(&s, data + i);
 uint8_t tail[128] = {0};
 size_t rest = size - i;
 memcpy(tail, data + i, rest);
 tail[rest] = 0x80;
 size_t total = rest + 9 <= 64 ? 64 : 128;
 uint64_t bits = (uint64_t)size * 8;
 for (int k = 0; k < 8; k++) tail[total - 1 - k] = (uint8_t)(bits >> (8 * k));
 sha256_block(&s, tail);
 if (total == 128) sha256_block(&s, tail + 64);
 for (int k = 0; k < 8; k++) {
  out[4 * k] = (uint8_t)(s.state[k] >> 24); out[4 * k + 1] = (uint8_t)(s.state[k] >> 16);
  out[4 * k + 2] = (uint8_t)(s.state[k] >> 8); out[4 * k + 3] = (uint8_t)s.state[k];
 }
}

static int wide_equals(LPCWSTR a, const char *b) {
 for (; *a && *b; a++, b++) if ((uint32_t)*a != (unsigned char)*b) return 0;
 return !*a && !*b;
}
NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *algorithm, LPCWSTR id, LPCWSTR implementation, ULONG flags) {
 (void)implementation; (void)flags;
 if (wide_equals(id, "SHA256")) { *algorithm = (BCRYPT_ALG_HANDLE)(uintptr_t)1; return STATUS_SUCCESS; }
 if (wide_equals(id, "RNG")) { *algorithm = (BCRYPT_ALG_HANDLE)(uintptr_t)2; return STATUS_SUCCESS; }
 return STATUS_NOT_SUPPORTED;
}
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE algorithm, ULONG flags) { (void)algorithm; (void)flags; return STATUS_SUCCESS; }
NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer, ULONG size, ULONG flags) {
 (void)algorithm; (void)flags;
 for (ULONG done = 0; done < size;) {
  ssize_t n = getrandom(buffer + done, size - done, 0);
  if (n < 0) { if (errno == EINTR) continue; return STATUS_UNSUCCESSFUL; }
  done += (ULONG)n;
 }
 return STATUS_SUCCESS;
}
NTSTATUS BCryptHash(BCRYPT_ALG_HANDLE algorithm, PUCHAR secret, ULONG secret_size, PUCHAR input, ULONG input_size,
                    PUCHAR output, ULONG output_size) {
 if (algorithm != (BCRYPT_ALG_HANDLE)(uintptr_t)1 || secret || secret_size || output_size != 32) return STATUS_INVALID_PARAMETER;
 sha256(input, input_size, output);
 return STATUS_SUCCESS;
}
