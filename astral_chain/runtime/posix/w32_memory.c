/* Linux host: Win32 virtual memory, write watch, file mappings and placeholders, vectored exception handling and
 * modules.
 *
 * Reservations are PROT_NONE MAP_NORESERVE mappings; commit is mprotect, decommit MADV_DONTNEED and PROT_NONE. Linux
 * keeps no committed/reserved distinction, so a byte per page of the guest's 39-bit address space records it (mapped
 * lazily: only touched pages cost memory). Every allocation is also a record (base, size, kind), which VirtualFree,
 * VirtualQuery and the placeholder calls work from.
 *
 * Write watch (MEM_WRITE_WATCH, GetWriteWatch): a userfaultfd in asynchronous write-protect mode (Linux 6.7). The
 * kernel clears a page's protection at its first write without a round trip to us, and PAGEMAP_SCAN lists the pages
 * whose protection was cleared and protects them again (the approach of TotK Recomp's runtime/platform/linux.c, MIT).
 * Kernel writes into such pages (read() into a guest buffer) are recorded as well. Without it, every page counts as
 * written and the renderer re-uploads more than needed.
 *
 * Memory faults (SIGSEGV, SIGBUS, SIGILL, SIGFPE) run the vectored handlers with an EXCEPTION_RECORD and a CONTEXT
 * made from the signal's ucontext; a handler that continues execution resumes at the context's Rip. */
#include "w32_internal.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef PAGEMAP_SCAN
struct page_region { uint64_t start, end, categories; };
struct pm_scan_arg {
 uint64_t size, flags, start, end, walk_end, vec, vec_len, max_pages, category_inverted, category_mask,
          category_anyof_mask, return_mask;
};
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WRITTEN (1 << 1)
#define PAGE_IS_PRESENT (1 << 3)
#define PAGE_IS_SWAPPED (1 << 4)
#define PAGE_IS_PFNZERO (1 << 5)
#define PM_SCAN_WP_MATCHING (1 << 0)
#define PM_SCAN_CHECK_WPASYNC (1 << 1)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PG 4096ull
#define PS_LIMIT (1ull << 39)
enum { PS_FREE, PS_RESERVED, PS_NONE, PS_READ, PS_RW };
enum { REC_ALLOC = 1, REC_PLACEHOLDER, REC_VIEW };
typedef struct { uint64_t base, size; int kind; int watched; DWORD protect; } Record;

static uint8_t *pstate;
static Record *records;
static unsigned record_count, record_capacity;
static int ww_fd = -1, pm_fd = -1, ww_tried;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

static int prot_of(DWORD protect) {
 switch (protect & 0xFF) {
 case PAGE_READONLY: case PAGE_EXECUTE_READ: case PAGE_EXECUTE: return PROT_READ;
 case PAGE_READWRITE: case PAGE_WRITECOPY: case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE;
 default: return PROT_NONE;
 }
}
static uint8_t ps_of(DWORD protect) {
 int p = prot_of(protect);
 return p == (PROT_READ | PROT_WRITE) ? PS_RW : p == PROT_READ ? PS_READ : PS_NONE;
}
static DWORD protect_of_ps(uint8_t v) {
 return v == PS_RW ? PAGE_READWRITE : v == PS_READ ? PAGE_READONLY : PAGE_NOACCESS;
}
static void ps_init(void) {
 if (pstate) return;
 void *p = mmap(NULL, PS_LIMIT / PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
 pstate = p == MAP_FAILED ? NULL : p;
}
static void ps_set(uint64_t addr, uint64_t n, uint8_t v) {
 if (!pstate || addr >= PS_LIMIT) return;
 uint64_t end = addr + n > PS_LIMIT ? PS_LIMIT : addr + n;
 memset(pstate + addr / PG, v, (size_t)((end - addr + PG - 1) / PG));
}

static Record *record_find(uint64_t addr) {
 for (unsigned i = 0; i < record_count; i++)
  if (addr >= records[i].base && addr - records[i].base < records[i].size) return &records[i];
 return NULL;
}
static Record *record_add(uint64_t base, uint64_t size, int kind, DWORD protect) {
 if (record_count == record_capacity) {
  unsigned capacity = record_capacity ? record_capacity * 2 : 256;
  Record *grown = realloc(records, capacity * sizeof(Record));
  if (!grown) return NULL;
  records = grown;
  record_capacity = capacity;
 }
 Record *r = &records[record_count++];
 *r = (Record){base, size, kind, 0, protect};
 return r;
}
static void record_remove(Record *r) { *r = records[--record_count]; }

static void ww_open(void) {
 ww_tried = 1;
 int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
 struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
 if (fd < 0 || ioctl(fd, UFFDIO_API, &api)) {
  fprintf(stderr, "[host] no asynchronous userfaultfd write protection (%s; Linux 6.7+ needed): written memory is "
          "not tracked\n", strerror(errno));
  if (fd >= 0) close(fd);
  return;
 }
 pm_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
 if (pm_fd < 0) { close(fd); return; }
 ww_fd = fd;
}

static void ww_register(uint64_t addr, uint64_t n) {
 if (!ww_tried) ww_open();
 madvise((void *)(uintptr_t)addr, n, MADV_NOHUGEPAGE);
 if (ww_fd < 0) return;
 struct uffdio_register reg = {.range = {addr, n}, .mode = UFFDIO_REGISTER_MODE_WP};
 if (ioctl(ww_fd, UFFDIO_REGISTER, &reg))
  fprintf(stderr, "[host] write watch over 0x%llx+0x%llx failed (%s)\n", (unsigned long long)addr,
          (unsigned long long)n, strerror(errno));
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
 if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
 pthread_mutex_lock(&vm_lock);
 ps_init();
 uint64_t base = (uint64_t)(uintptr_t)address;
 void *result = NULL;
 if (type & MEM_RESERVE) {
  uint64_t start = base & ~(PG - 1), length = (base + size - start + PG - 1) & ~(PG - 1);
  int commit = (type & MEM_COMMIT) != 0;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (start ? MAP_FIXED_NOREPLACE : 0);
  void *p = mmap(start ? (void *)(uintptr_t)start : NULL, length, commit && !(type & MEM_WRITE_WATCH) ? prot_of(protect) : PROT_NONE,
                 flags, -1, 0);
  if (p == MAP_FAILED || (start && p != (void *)(uintptr_t)start)) {
   if (p != MAP_FAILED) munmap(p, length);
   SetLastError(ERROR_INVALID_ADDRESS);
   goto done;
  }
  start = (uint64_t)(uintptr_t)p;
  Record *r = record_add(start, length, REC_ALLOC, commit ? protect : PAGE_NOACCESS);
  if (!r) { munmap(p, length); SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto done; }
  ps_set(start, length, PS_RESERVED);
  if (type & MEM_WRITE_WATCH) { r->watched = 1; ww_register(start, length); }
  if (commit) {
   if ((type & MEM_WRITE_WATCH) && mprotect(p, length, prot_of(protect))) { w32_set_errno_error(); goto done; }
   ps_set(start, length, ps_of(protect));
  }
  result = p;
 } else if (type & MEM_COMMIT) {
  uint64_t start = base & ~(PG - 1), end = (base + size + PG - 1) & ~(PG - 1);
  Record *r = record_find(start);
  if (!r || end - r->base > r->size) { SetLastError(ERROR_INVALID_ADDRESS); goto done; }
  if (mprotect((void *)(uintptr_t)start, end - start, prot_of(protect))) { w32_set_errno_error(); goto done; }
  ps_set(start, end - start, ps_of(protect));
  result = (void *)(uintptr_t)start;
 } else SetLastError(ERROR_INVALID_PARAMETER);
done:
 pthread_mutex_unlock(&vm_lock);
 return result;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
 uint64_t base = (uint64_t)(uintptr_t)address;
 BOOL ok = FALSE;
 pthread_mutex_lock(&vm_lock);
 Record *r = record_find(base);
 if (!r) { SetLastError(ERROR_INVALID_ADDRESS); goto done; }
 if (type & MEM_RELEASE) {
  if (type & MEM_PRESERVE_PLACEHOLDER) {
   /* split a placeholder: [base, base+size) becomes its own placeholder */
   if (r->kind != REC_PLACEHOLDER || !size || size % PG || size > r->size - (base - r->base)) { SetLastError(ERROR_INVALID_PARAMETER); goto done; }
   if (base == r->base && size == r->size) { ok = TRUE; goto done; }
   Record old = *r;
   record_remove(r);
   if (base > old.base) record_add(old.base, base - old.base, REC_PLACEHOLDER, PAGE_NOACCESS);
   record_add(base, size, REC_PLACEHOLDER, PAGE_NOACCESS);
   if (base + size < old.base + old.size) record_add(base + size, old.base + old.size - base - size, REC_PLACEHOLDER, PAGE_NOACCESS);
   ok = TRUE;
   goto done;
  }
  if (size || base != r->base) { SetLastError(ERROR_INVALID_PARAMETER); goto done; }
  if (munmap((void *)(uintptr_t)r->base, r->size)) { w32_set_errno_error(); goto done; }
  ps_set(r->base, r->size, PS_FREE);
  record_remove(r);
  ok = TRUE;
 } else if (type & MEM_DECOMMIT) {
  uint64_t start = base & ~(PG - 1), end = size ? (base + size + PG - 1) & ~(PG - 1) : r->base + r->size;
  if (end - r->base > r->size) { SetLastError(ERROR_INVALID_PARAMETER); goto done; }
  if (madvise((void *)(uintptr_t)start, end - start, MADV_DONTNEED) ||
      mprotect((void *)(uintptr_t)start, end - start, PROT_NONE)) { w32_set_errno_error(); goto done; }
  ps_set(start, end - start, PS_RESERVED);
  ok = TRUE;
 } else SetLastError(ERROR_INVALID_PARAMETER);
done:
 pthread_mutex_unlock(&vm_lock);
 return ok;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old_protect) {
 uint64_t start = (uint64_t)(uintptr_t)address & ~(PG - 1), end = ((uint64_t)(uintptr_t)address + size + PG - 1) & ~(PG - 1);
 pthread_mutex_lock(&vm_lock);
 if (pstate && end <= PS_LIMIT)
  for (uint64_t a = start; a < end; a += PG)
   if (pstate[a / PG] < PS_NONE) { pthread_mutex_unlock(&vm_lock); SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
 DWORD old = PAGE_READWRITE;
 if (pstate && start < PS_LIMIT) old = protect_of_ps(pstate[start / PG]);
 else { Record *r = record_find(start); if (r) old = r->protect; }
 int failed = mprotect((void *)(uintptr_t)start, end - start, prot_of(protect));
 if (!failed) {
  if (start < PS_LIMIT) ps_set(start, end - start, ps_of(protect));
  else { Record *r = record_find(start); if (r && r->base == start && r->size == end - start) r->protect = protect; }
 }
 pthread_mutex_unlock(&vm_lock);
 if (failed) { w32_set_errno_error(); return FALSE; }
 if (old_protect) *old_protect = old;
 return TRUE;
}

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION information, SIZE_T size) {
 if (size < sizeof(*information)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
 uint64_t addr = (uint64_t)(uintptr_t)address & ~(PG - 1);
 memset(information, 0, sizeof(*information));
 pthread_mutex_lock(&vm_lock);
 Record *r = record_find(addr);
 if (!r) {
  uint64_t next = UINT64_MAX;
  for (unsigned i = 0; i < record_count; i++) if (records[i].base > addr && records[i].base < next) next = records[i].base;
  information->BaseAddress = (PVOID)(uintptr_t)addr;
  information->RegionSize = (next == UINT64_MAX ? PG : next - addr);
  information->State = MEM_FREE;
  information->Protect = PAGE_NOACCESS;
 } else {
  uint64_t end = r->base + r->size, run_end = end;
  uint8_t v = PS_RW;
  if (pstate && addr < PS_LIMIT) {
   v = pstate[addr / PG];
   uint64_t i = addr / PG;
   while ((i + 1) * PG < end && pstate[i + 1] == v) i++;
   run_end = (i + 1) * PG;
  } else v = ps_of(r->protect);
  if (r->kind == REC_PLACEHOLDER) v = PS_RESERVED;
  information->BaseAddress = (PVOID)(uintptr_t)addr;
  information->AllocationBase = (PVOID)(uintptr_t)r->base;
  information->AllocationProtect = r->protect;
  information->RegionSize = run_end - addr;
  information->State = v == PS_RESERVED || v == PS_FREE ? MEM_RESERVE : MEM_COMMIT;
  information->Protect = information->State == MEM_COMMIT ? protect_of_ps(v) : 0;
  information->Type = r->kind == REC_VIEW ? MEM_MAPPED : MEM_PRIVATE;
 }
 pthread_mutex_unlock(&vm_lock);
 return sizeof(*information);
}

static uint64_t written_scan(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
 struct page_region r[256];
 uint64_t got = 0;
 for (uint64_t at = addr, end = addr + n; at < end && got < max;) {
  /* Written: protection cleared, on a populated page (present or swapped out) that is not the shared zero page. */
  struct pm_scan_arg arg = {.size = sizeof arg, .flags = reset ? PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC : 0,
                            .start = at, .end = end, .vec = (uint64_t)(uintptr_t)r, .vec_len = 256,
                            .max_pages = max - got, .category_inverted = PAGE_IS_PFNZERO,
                            .category_mask = PAGE_IS_WRITTEN | PAGE_IS_PFNZERO,
                            .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED, .return_mask = PAGE_IS_WRITTEN};
  long k = ioctl(pm_fd, PAGEMAP_SCAN, &arg);
  if (k < 0) return UINT64_MAX;
  for (long i = 0; i < k; i++)
   for (uint64_t p = r[i].start; p < r[i].end && got < max; p += PG) pages[got++] = p;
  if (arg.walk_end <= at) break;
  at = arg.walk_end;
 }
 return got;
}

UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addresses, ULONG_PTR *count, PULONG granularity) {
 uint64_t start = (uint64_t)(uintptr_t)base & ~(PG - 1), end = ((uint64_t)(uintptr_t)base + size + PG - 1) & ~(PG - 1);
 if (granularity) *granularity = (ULONG)PG;
 if (!addresses || !count) return ERROR_INVALID_PARAMETER;
 uint64_t max = *count, got;
 if (ww_fd >= 0) {
  got = written_scan(start, end - start, (uint64_t *)addresses, max, (flags & WRITE_WATCH_FLAG_RESET) != 0);
  if (got == UINT64_MAX) return ERROR_INVALID_PARAMETER;
 } else {
  /* untracked: every committed page counts as written */
  got = 0;
  for (uint64_t a = start; a < end && got < max; a += PG)
   if (!pstate || a >= PS_LIMIT || pstate[a / PG] >= PS_NONE) addresses[got++] = (PVOID)(uintptr_t)a;
 }
 *count = (ULONG_PTR)got;
 return 0;
}
UINT ResetWriteWatch(LPVOID base, SIZE_T size) {
 if (ww_fd < 0) return 0;
 static uint64_t scratch[65536];
 uint64_t start = (uint64_t)(uintptr_t)base & ~(PG - 1), end = ((uint64_t)(uintptr_t)base + size + PG - 1) & ~(PG - 1);
 for (uint64_t a = start; a < end;) {
  uint64_t e = end - a > 65536 * PG ? a + 65536 * PG : end;
  written_scan(a, e - a, scratch, 65536, 1);
  a = e;
 }
 return 0;
}
BOOL FlushInstructionCache(HANDLE process, LPCVOID address, SIZE_T size) { (void)process; (void)address; (void)size; return TRUE; }

/* ------------------------------------------------------------------------------------------------ file mappings */
static HANDLE create_mapping(HANDLE file, DWORD protect, uint64_t size) {
 int fd;
 if (file == INVALID_HANDLE_VALUE || !file) {
  fd = (int)syscall(SYS_memfd_create, "w32-section", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)size)) { w32_set_errno_error(); if (fd >= 0) close(fd); return NULL; }
 } else {
  W32Object *f = w32_object(file, W32_FILE);
  if (!f) return NULL;
  fd = fcntl(f->u.file.fd, F_DUPFD_CLOEXEC, 0);
  if (fd < 0) { w32_set_errno_error(); return NULL; }
  struct stat st;
  if (!size && !fstat(fd, &st)) size = (uint64_t)st.st_size;
  else if (size && !fstat(fd, &st) && (uint64_t)st.st_size < size && ftruncate(fd, (off_t)size)) {
   w32_set_errno_error(); close(fd); return NULL;
  }
 }
 W32Object *object = w32_object_new(W32_MAPPING);
 object->u.mapping.fd = fd;
 object->u.mapping.size = size;
 object->u.mapping.protect = protect;
 return w32_handle_new(object);
}
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect, DWORD size_high, DWORD size_low,
                          LPCWSTR name) {
 (void)attributes; (void)name;
 return create_mapping(file, protect, (uint64_t)size_high << 32 | size_low);
}
HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect, DWORD size_high, DWORD size_low,
                          LPCSTR name) {
 (void)attributes; (void)name;
 return create_mapping(file, protect, (uint64_t)size_high << 32 | size_low);
}

static void *map_view(W32Object *mapping, void *address, uint64_t offset, uint64_t size, int prot, int private_copy,
                      int fixed) {
 if (!size) size = mapping->u.mapping.size - offset;
 void *p = mmap(address, size, prot, (private_copy ? MAP_PRIVATE : MAP_SHARED) | (fixed ? MAP_FIXED : 0),
                mapping->u.mapping.fd, (off_t)offset);
 if (p == MAP_FAILED) { w32_set_errno_error(); return NULL; }
 return p;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low, SIZE_T size) {
 W32Object *object = w32_object(mapping, W32_MAPPING);
 if (!object) return NULL;
 uint64_t offset = (uint64_t)offset_high << 32 | offset_low;
 int prot = (access & (FILE_MAP_WRITE | FILE_MAP_COPY)) || access == FILE_MAP_ALL_ACCESS ? PROT_READ | PROT_WRITE : PROT_READ;
 if (!size) size = object->u.mapping.size - offset;
 void *p = map_view(object, NULL, offset, size, prot, access == FILE_MAP_COPY, 0);
 if (!p) return NULL;
 pthread_mutex_lock(&vm_lock);
 ps_init();
 record_add((uint64_t)(uintptr_t)p, (size + PG - 1) & ~(PG - 1), REC_VIEW, prot & PROT_WRITE ? PAGE_READWRITE : PAGE_READONLY);
 ps_set((uint64_t)(uintptr_t)p, size, prot & PROT_WRITE ? PS_RW : PS_READ);
 pthread_mutex_unlock(&vm_lock);
 return p;
}

BOOL UnmapViewOfFile(LPCVOID address) {
 pthread_mutex_lock(&vm_lock);
 Record *r = record_find((uint64_t)(uintptr_t)address);
 BOOL ok = FALSE;
 if (r && r->kind == REC_VIEW && r->base == (uint64_t)(uintptr_t)address) {
  munmap((void *)(uintptr_t)r->base, r->size);
  ps_set(r->base, r->size, PS_FREE);
  record_remove(r);
  ok = TRUE;
 } else SetLastError(ERROR_INVALID_ADDRESS);
 pthread_mutex_unlock(&vm_lock);
 return ok;
}

/* KernelBase placeholders: a placeholder is a PROT_NONE reservation; a view replaces it in place (MAP_FIXED). */
PVOID VirtualAlloc2(HANDLE process, PVOID address, SIZE_T size, ULONG type, ULONG protect, MEM_EXTENDED_PARAMETER *params,
                    ULONG count) {
 (void)process; (void)params; (void)count;
 if (!(type & MEM_RESERVE_PLACEHOLDER)) return VirtualAlloc(address, size, type, protect);
 uint64_t base = (uint64_t)(uintptr_t)address;
 if (!base || base % PG || !size || size % PG) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
 pthread_mutex_lock(&vm_lock);
 ps_init();
 void *p = mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
 if (p != address) {
  if (p != MAP_FAILED) munmap(p, size);
  pthread_mutex_unlock(&vm_lock);
  SetLastError(ERROR_INVALID_ADDRESS);
  return NULL;
 }
 record_add(base, size, REC_PLACEHOLDER, PAGE_NOACCESS);
 ps_set(base, size, PS_RESERVED);
 pthread_mutex_unlock(&vm_lock);
 return p;
}

PVOID MapViewOfFile3(HANDLE mapping, HANDLE process, PVOID address, ULONG64 offset, SIZE_T size, ULONG type, ULONG protect,
                     MEM_EXTENDED_PARAMETER *params, ULONG count) {
 (void)process; (void)params; (void)count;
 W32Object *object = w32_object(mapping, W32_MAPPING);
 if (!object) return NULL;
 int prot = prot_of(protect);
 if (!(type & MEM_REPLACE_PLACEHOLDER)) {
  void *p = map_view(object, NULL, offset, size, prot, 0, 0);
  if (!p) return NULL;
  pthread_mutex_lock(&vm_lock);
  ps_init();
  record_add((uint64_t)(uintptr_t)p, (size + PG - 1) & ~(PG - 1), REC_VIEW, protect);
  ps_set((uint64_t)(uintptr_t)p, size, ps_of(protect));
  pthread_mutex_unlock(&vm_lock);
  return p;
 }
 pthread_mutex_lock(&vm_lock);
 Record *r = record_find((uint64_t)(uintptr_t)address);
 void *p = NULL;
 if (!r || r->kind != REC_PLACEHOLDER || r->base != (uint64_t)(uintptr_t)address || r->size != size) {
  SetLastError(ERROR_INVALID_ADDRESS);
 } else if ((p = map_view(object, address, offset, size, prot, 0, 1))) {
  r->kind = REC_VIEW;
  r->protect = protect;
  ps_set(r->base, r->size, ps_of(protect));
 }
 pthread_mutex_unlock(&vm_lock);
 return p;
}

BOOL UnmapViewOfFile2(HANDLE process, PVOID address, ULONG flags) {
 (void)process;
 if (!(flags & MEM_PRESERVE_PLACEHOLDER)) return UnmapViewOfFile(address);
 pthread_mutex_lock(&vm_lock);
 Record *r = record_find((uint64_t)(uintptr_t)address);
 BOOL ok = FALSE;
 if (r && r->kind == REC_VIEW && r->base == (uint64_t)(uintptr_t)address) {
  void *p = mmap(address, r->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  if (p == address) {
   r->kind = REC_PLACEHOLDER;
   r->protect = PAGE_NOACCESS;
   ps_set(r->base, r->size, PS_RESERVED);
   ok = TRUE;
  } else w32_set_errno_error();
 } else SetLastError(ERROR_INVALID_ADDRESS);
 pthread_mutex_unlock(&vm_lock);
 return ok;
}

/* ------------------------------------------------------------------------------------------------ exceptions */
#define MAX_HANDLERS 16
static PVECTORED_EXCEPTION_HANDLER handlers[MAX_HANDLERS];
static unsigned handler_count;
static LPTOP_LEVEL_EXCEPTION_FILTER unhandled_filter;
static pthread_mutex_t handler_lock = PTHREAD_MUTEX_INITIALIZER;
static int faults_installed;

static void fault_handler(int sig, siginfo_t *si, void *uc_void);

static void install_faults(void) {
 if (faults_installed) return;
 faults_installed = 1;
 struct sigaction sa;
 memset(&sa, 0, sizeof sa);
 sa.sa_sigaction = fault_handler;
 sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
 sigemptyset(&sa.sa_mask);
 sigaction(SIGSEGV, &sa, NULL);
 sigaction(SIGBUS, &sa, NULL);
 sigaction(SIGILL, &sa, NULL);
 sigaction(SIGFPE, &sa, NULL);
}

void w32_thread_signal_setup(void) {
 static __thread int done;
 if (done) return;
 done = 1;
 size_t size = 256 * 1024;
 void *stack = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
 if (stack == MAP_FAILED) return;
 stack_t ss = {.ss_sp = stack, .ss_size = size, .ss_flags = 0};
 sigaltstack(&ss, NULL);
}

__attribute__((constructor)) static void w32_memory_init(void) {
 /* a write to a closed pipe fails (ERROR_BROKEN_PIPE / EPIPE) as on Windows instead of ending the process */
 signal(SIGPIPE, SIG_IGN);
 w32_thread_signal_setup();
 install_faults();
}

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler) {
 pthread_mutex_lock(&handler_lock);
 install_faults();
 PVOID result = NULL;
 if (handler_count < MAX_HANDLERS) {
  if (first) {
   memmove(handlers + 1, handlers, handler_count * sizeof(handlers[0]));
   handlers[0] = handler;
  } else handlers[handler_count] = handler;
  handler_count++;
  result = (PVOID)handler;
 }
 pthread_mutex_unlock(&handler_lock);
 return result;
}
ULONG RemoveVectoredExceptionHandler(PVOID handle) {
 pthread_mutex_lock(&handler_lock);
 ULONG removed = 0;
 for (unsigned i = 0; i < handler_count; i++) if ((PVOID)handlers[i] == handle) {
  memmove(handlers + i, handlers + i + 1, (handler_count - i - 1) * sizeof(handlers[0]));
  handler_count--;
  removed = 1;
  break;
 }
 pthread_mutex_unlock(&handler_lock);
 return removed;
}
LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
 install_faults();
 LPTOP_LEVEL_EXCEPTION_FILTER old = unhandled_filter;
 unhandled_filter = filter;
 return old;
}

static void fault_handler(int sig, siginfo_t *si, void *uc_void) {
 ucontext_t *uc = uc_void;
 greg_t *g = uc->uc_mcontext.gregs;
 int saved = errno;
 EXCEPTION_RECORD record;
 memset(&record, 0, sizeof record);
 CONTEXT context;
 memset(&context, 0, sizeof context);
 context.ContextFlags = CONTEXT_FULL;
 context.Rax = (DWORD64)g[REG_RAX]; context.Rcx = (DWORD64)g[REG_RCX]; context.Rdx = (DWORD64)g[REG_RDX];
 context.Rbx = (DWORD64)g[REG_RBX]; context.Rsp = (DWORD64)g[REG_RSP]; context.Rbp = (DWORD64)g[REG_RBP];
 context.Rsi = (DWORD64)g[REG_RSI]; context.Rdi = (DWORD64)g[REG_RDI]; context.R8 = (DWORD64)g[REG_R8];
 context.R9 = (DWORD64)g[REG_R9]; context.R10 = (DWORD64)g[REG_R10]; context.R11 = (DWORD64)g[REG_R11];
 context.R12 = (DWORD64)g[REG_R12]; context.R13 = (DWORD64)g[REG_R13]; context.R14 = (DWORD64)g[REG_R14];
 context.R15 = (DWORD64)g[REG_R15]; context.Rip = (DWORD64)g[REG_RIP];
 record.ExceptionAddress = (PVOID)(uintptr_t)g[REG_RIP];
 switch (sig) {
 case SIGSEGV: case SIGBUS:
  record.ExceptionCode = sig == SIGBUS ? EXCEPTION_IN_PAGE_ERROR : EXCEPTION_ACCESS_VIOLATION;
  record.NumberParameters = 2;
  record.ExceptionInformation[0] = (g[REG_ERR] & 16) ? 8 : (g[REG_ERR] & 2) ? 1 : 0;
  record.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
  break;
 case SIGILL: record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION; break;
 default: record.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO; break;
 }
 EXCEPTION_POINTERS pointers = {&record, &context};
 unsigned count = handler_count;
 for (unsigned i = 0; i < count && i < MAX_HANDLERS; i++) {
  PVECTORED_EXCEPTION_HANDLER h = handlers[i];
  if (h && h(&pointers) == EXCEPTION_CONTINUE_EXECUTION) {
   g[REG_RIP] = (greg_t)context.Rip; g[REG_RSP] = (greg_t)context.Rsp; g[REG_RAX] = (greg_t)context.Rax;
   errno = saved;
   return;
  }
 }
 if (unhandled_filter) {
  LONG r = unhandled_filter(&pointers);
  if (r == EXCEPTION_CONTINUE_EXECUTION) { g[REG_RIP] = (greg_t)context.Rip; errno = saved; return; }
  if (r == EXCEPTION_EXECUTE_HANDLER) { fflush(stdout); fflush(stderr); _exit((int)record.ExceptionCode); }
 }
 /* default action: the instruction faults again and the process dumps core */
 struct sigaction sa;
 memset(&sa, 0, sizeof sa);
 sa.sa_handler = SIG_DFL;
 sigaction(sig, &sa, NULL);
 errno = saved;
}

/* ------------------------------------------------------------------------------------------------ modules */
#define MODULE_MAGIC 0x4D4F4455u
typedef struct { uint32_t magic; int builtin; void *dl; char name[64]; } Module;

static void *main_base(void) {
 Dl_info info;
 return dladdr((void *)main_base, &info) ? info.dli_fbase : NULL;
}

static Module *module_of(HMODULE handle) {
 Module *m = handle;
 return m && m != main_base() && m->magic == MODULE_MAGIC ? m : NULL;
}

static void lower_leaf(const char *name, char *out, size_t size) {
 const char *leaf = name;
 for (const char *c = name; *c; c++) if (*c == '\\' || *c == '/') leaf = c + 1;
 size_t i = 0;
 for (; leaf[i] && i + 1 < size; i++) out[i] = (char)(leaf[i] >= 'A' && leaf[i] <= 'Z' ? leaf[i] + 32 : leaf[i]);
 out[i] = 0;
}

static HMODULE load_library(const char *name) {
 char leaf[64];
 lower_leaf(name, leaf, sizeof leaf);
 Module *m = calloc(1, sizeof(Module));
 if (!m) return NULL;
 m->magic = MODULE_MAGIC;
 snprintf(m->name, sizeof m->name, "%s", leaf);
 if (w32_builtin_module(leaf) || !strcmp(leaf, "kernelbase.dll") || !strcmp(leaf, "kernel32.dll")) {
  m->builtin = 1;
  return m;
 }
 if (!strcmp(leaf, "vulkan-1.dll")) m->dl = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
 else if (!strcmp(leaf, "dbghelp.dll")) m->dl = NULL;
 else {
  char *path = w32_path_from_ansi(name);
  size_t n = path ? strlen(path) : 0;
  if (n > 4 && !strcasecmp(path + n - 4, ".dll")) strcpy(path + n - 4, ".so");
  m->dl = path ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
  free(path);
 }
 if (!m->dl) { free(m); SetLastError(ERROR_MOD_NOT_FOUND); return NULL; }
 return m;
}

HMODULE LoadLibraryA(LPCSTR name) { return name ? load_library(name) : NULL; }
HMODULE LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) { (void)file; (void)flags; return LoadLibraryA(name); }
HMODULE LoadLibraryW(LPCWSTR name) {
 char *n = w32_utf8_from_wide(name, -1);
 HMODULE m = n ? load_library(n) : NULL;
 free(n);
 return m;
}
HMODULE LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) { (void)file; (void)flags; return LoadLibraryW(name); }

HMODULE GetModuleHandleA(LPCSTR name) {
 if (!name) return main_base();
 char leaf[64];
 lower_leaf(name, leaf, sizeof leaf);
 if (!strcmp(leaf, "kernelbase.dll") || !strcmp(leaf, "kernel32.dll") || w32_builtin_module(leaf)) return load_library(name);
 return NULL;
}
HMODULE GetModuleHandleW(LPCWSTR name) {
 if (!name) return main_base();
 char *n = w32_utf8_from_wide(name, -1);
 HMODULE m = n ? GetModuleHandleA(n) : NULL;
 free(n);
 return m;
}
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE *module) {
 if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
  Dl_info info;
  if (!dladdr((const void *)name, &info) || !info.dli_fbase) { SetLastError(ERROR_MOD_NOT_FOUND); return FALSE; }
  *module = info.dli_fbase;
  return TRUE;
 }
 *module = GetModuleHandleA(name);
 return *module != NULL;
}

FARPROC GetProcAddress(HMODULE module, LPCSTR name) {
 void *p = NULL;
 Module *m = module_of(module);
 if (!m) p = dlsym(RTLD_DEFAULT, name);
 else if (m->builtin) {
  if (!strcmp(m->name, "kernelbase.dll") || !strcmp(m->name, "kernel32.dll")) {
   if (!strcmp(name, "VirtualAlloc2")) p = (void *)VirtualAlloc2;
   else if (!strcmp(name, "MapViewOfFile3")) p = (void *)MapViewOfFile3;
   else if (!strcmp(name, "UnmapViewOfFile2")) p = (void *)UnmapViewOfFile2;
   else if (!strcmp(name, "GetCurrentThreadStackLimits")) p = (void *)GetCurrentThreadStackLimits;
  } else p = (void *)w32_builtin_proc(m->name, name);
 } else p = dlsym(m->dl, name);
 if (!p) SetLastError(ERROR_PROC_NOT_FOUND);
 return (FARPROC)p;
}

BOOL FreeLibrary(HMODULE module) {
 Module *m = module_of(module);
 if (!m) return TRUE;
 if (m->dl) dlclose(m->dl);
 m->magic = 0;
 free(m);
 return TRUE;
}
