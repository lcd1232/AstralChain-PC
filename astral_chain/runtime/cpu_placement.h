/* Thread placement on hybrid Intel CPUs (performance and efficiency cores; Linux).
 *
 * The scheduler spreads the game's busy host threads over every core, and the efficiency cores are much slower: in a
 * busy scene on a Core Ultra X7 358H (4 performance cores 0-3, 12 efficiency cores) the frame waits for one guest worker
 * thread while another spins on it, and running that worker, the main thread and the render worker on the performance
 * cores took 41.8 fps to 52.6 (2026-11-02). Which guest thread does the work changes with the scene (several have the
 * same priority and core), so a monitor measures it: once a second each registered thread's CPU time, less the time
 * it spent in the game's spin waits (native_trace.py adds their time stamp counter cycles to rt_spin_tsc), ranks the
 * threads; the
 * busiest (one fewer than the performance cores, a current one stays unless another does 30% more) run on the
 * performance cores, every other registered thread on the efficiency cores.
 *
 * AC_CPU_PLACEMENT=0: off (the scheduler places every thread). Off without /sys/devices/cpu_core (not a hybrid Intel
 * CPU: the Steam Deck, desktop AMD). */
/* native_trace.py: each turn of a spin wait adds the cycles since the previous turn (when under 20000: the same wait)
 * to rt_spin_tsc */
__thread unsigned long long rt_spin_tsc, rt_spin_last;
#ifdef __linux__
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dirent.h>
#define CPU_PLACE_SLOTS 40
static struct { volatile int tid; volatile unsigned long long *spins; const char *name; unsigned slot;
                unsigned long long ticks, spin_count; int on_p; } cpu_place[CPU_PLACE_SLOTS];
typedef struct { unsigned long bits[16]; } cpu_place_set;  /* (sched_setaffinity by syscall: no _GNU_SOURCE here) */
static cpu_place_set cpu_place_p, cpu_place_e;
static int cpu_place_state;  /* 0 not started, 1 running, -1 off */
static pthread_mutex_t cpu_place_lock = PTHREAD_MUTEX_INITIALIZER;
static int cpu_place_list(const char *path, cpu_place_set *set) {
 FILE *f = fopen(path, "r"); if (!f) return 0;
 char line[256]; int n = 0; memset(set, 0, sizeof *set);
 if (fgets(line, sizeof line, f)) for (char *s = line; *s && *s != '\n';) {
  char *e; long a = strtol(s, &e, 10), b = a; if (e == s) break;
  if (*e == '-') b = strtol(e + 1, &e, 10);
  for (long i = a; i <= b && i < 1024; i++) { set->bits[i / 64] |= 1ul << (i % 64); n++; }
  s = *e == ',' ? e + 1 : e;
 }
 fclose(f); return n;
}
static unsigned long long cpu_place_ticks(int tid) {
 char path[64], buf[512]; snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
 FILE *f = fopen(path, "r"); if (!f) return 0;
 size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
 char *s = strrchr(buf, ')'); if (!s) return 0;
 unsigned long long ut = 0, st = 0;  /* fields 14 and 15: after ") state" come 11 more before utime */
 if (sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) return 0;
 return ut + st;
}
static void cpu_place_describe(char *out, size_t size) {
 size_t used = 0; out[0] = 0;
 for (int i = 0; i < CPU_PLACE_SLOTS; i++) if (cpu_place[i].tid && cpu_place[i].on_p == 1) {
  int n = cpu_place[i].name ? snprintf(out + used, size - used, "%s%s", used ? ", " : "", cpu_place[i].name)
                            : snprintf(out + used, size - used, "%sguest thread %u", used ? ", " : "", cpu_place[i].slot);
  if (n < 0 || (size_t)n >= size - used) break; used += n;
 }
}
/* Threads the game or the drivers start from a thread on the performance cores inherit its mask: unregistered ones
 * with exactly that mask get every core back. */
static void cpu_place_release(void) {
 DIR *d = opendir("/proc/self/task"); if (!d) return;
 for (struct dirent *e; (e = readdir(d));) {
  int tid = atoi(e->d_name); if (tid <= 0) continue;
  int known = 0; for (int i = 0; i < CPU_PLACE_SLOTS; i++) if (cpu_place[i].tid == tid) known = 1;
  if (known) continue;
  cpu_place_set mask; memset(&mask, 0, sizeof mask);
  if (syscall(SYS_sched_getaffinity, tid, sizeof mask, &mask) <= 0 || memcmp(&mask, &cpu_place_p, sizeof mask)) continue;
  cpu_place_set all; for (int i = 0; i < 16; i++) all.bits[i] = cpu_place_p.bits[i] | cpu_place_e.bits[i];
  syscall(SYS_sched_setaffinity, tid, sizeof all, &all);
 }
 closedir(d);
}
static void *cpu_place_monitor(void *arg) {
 (void)arg;
 int p_count = 0; for (int i = 0; i < 16; i++) p_count += __builtin_popcountl(cpu_place_p.bits[i]);
 int places = p_count - 1;
 double hz = (double)sysconf(_SC_CLK_TCK);
 LARGE_INTEGER f, then, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&then);
 unsigned long long tsc_then = __builtin_ia32_rdtsc();
 int verbose = getenv("AC_CPU_PLACEMENT_LOG") != NULL;
 for (;;) {
  struct timespec ts = {1, 0}; nanosleep(&ts, NULL);
  QueryPerformanceCounter(&now); unsigned long long tsc_now = __builtin_ia32_rdtsc();
  double tsc_hz = (double)(tsc_now - tsc_then) * (double)f.QuadPart / (double)(now.QuadPart - then.QuadPart);
  then = now; tsc_then = tsc_now;
  double work[CPU_PLACE_SLOTS];
  pthread_mutex_lock(&cpu_place_lock);
  for (int i = 0; i < CPU_PLACE_SLOTS; i++) {
   work[i] = -1; if (!cpu_place[i].tid) continue;
   unsigned long long t = cpu_place_ticks(cpu_place[i].tid);
   if (!t && cpu_place[i].ticks) { cpu_place[i].tid = 0; continue; }  /* (thread ended) */
   unsigned long long spins = cpu_place[i].spins ? *cpu_place[i].spins : 0;
   double cpu = (t - cpu_place[i].ticks) / hz, spin = (double)(spins - cpu_place[i].spin_count) / tsc_hz;
   cpu_place[i].ticks = t; cpu_place[i].spin_count = spins;
   work[i] = cpu - spin; if (work[i] < 0) work[i] = 0;
  }
  /* the busiest threads doing more than a quarter of a core; current ones stay unless outdone by 30% */
  int want[CPU_PLACE_SLOTS] = {0};
  for (int k = 0; k < places; k++) {
   int best = -1;
   for (int i = 0; i < CPU_PLACE_SLOTS; i++) {
    if (want[i] || work[i] < 0.25) continue;
    double w = work[i] * (cpu_place[i].on_p == 1 ? 1.3 : 1.0);
    if (best < 0 || w > work[best] * (cpu_place[best].on_p == 1 ? 1.3 : 1.0)) best = i;
   }
   if (best < 0) break;
   want[best] = 1;
  }
  int changed = 0;
  for (int i = 0; i < CPU_PLACE_SLOTS; i++) {
   if (!cpu_place[i].tid || cpu_place[i].on_p == want[i]) continue;
   cpu_place_set *set = want[i] ? &cpu_place_p : &cpu_place_e;
   if (!syscall(SYS_sched_setaffinity, cpu_place[i].tid, sizeof *set, set)) { if (cpu_place[i].on_p != want[i]) changed = 1;
    cpu_place[i].on_p = want[i]; }
  }
  if (changed) { char text[256]; cpu_place_describe(text, sizeof text);
   printf("[cpu] performance cores: %s\n", text[0] ? text : "-"); fflush(stdout); }
  if (verbose) {  /* AC_CPU_PLACEMENT_LOG: each thread's work (cores, less its spin waits) */
   printf("[cpu] work:");
   for (int i = 0; i < CPU_PLACE_SLOTS; i++) if (work[i] >= 0.05) printf(" %s%u=%.2f", cpu_place[i].name ? "r" : "g", cpu_place[i].slot, work[i]);
   printf("\n"); fflush(stdout);
  }
  cpu_place_release();
  pthread_mutex_unlock(&cpu_place_lock);
 }
 return NULL;
}
static void cpu_place_start(void) {
 const char *e = getenv("AC_CPU_PLACEMENT");
 int p = cpu_place_list("/sys/devices/cpu_core/cpus", &cpu_place_p), n = cpu_place_list("/sys/devices/cpu_atom/cpus", &cpu_place_e);
 if ((e && atoi(e) == 0) || p < 2 || n < 1) { cpu_place_state = -1; return; }
 pthread_t t;
 if (pthread_create(&t, NULL, cpu_place_monitor, NULL)) { cpu_place_state = -1; return; }
 pthread_detach(t);
 printf("[cpu] hybrid CPU: %d performance cores, %d efficiency cores\n", p, n);
 cpu_place_state = 1;
}
/* Called on the thread itself: guest thread slot (its spin waits counted), or name for a host thread (render worker). */
void cpu_place_register(unsigned slot, const char *name) {
 pthread_mutex_lock(&cpu_place_lock);
 if (!cpu_place_state) cpu_place_start();
 if (cpu_place_state > 0) {
  unsigned i = name ? CPU_PLACE_SLOTS - 1 : slot;
  if (i < CPU_PLACE_SLOTS) {
   cpu_place[i].tid = (int)syscall(SYS_gettid); cpu_place[i].spins = name ? NULL : &rt_spin_tsc;
   cpu_place[i].name = name; cpu_place[i].slot = slot; cpu_place[i].ticks = 0; cpu_place[i].spin_count = rt_spin_tsc;
   cpu_place[i].on_p = -1;
  }
 }
 pthread_mutex_unlock(&cpu_place_lock);
}
#else
void cpu_place_register(unsigned slot, const char *name) { (void)slot; (void)name; }
#endif
