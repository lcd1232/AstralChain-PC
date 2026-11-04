/* Linux host: the game's entry. The Windows build links a 64 MB main-thread stack (direct calls nest page functions
 * along guest calls); here the program's main (compiled as w32_program_main) runs on a thread with that stack. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

int w32_program_main(int argc, char **argv);
void w32_thread_signal_setup(void);

static int saved_argc, exit_code, returned;
static char **saved_argv;

static void *run(void *unused) {
 (void)unused;
 w32_thread_signal_setup();
 exit_code = w32_program_main(saved_argc, saved_argv);
 returned = 1;
 return NULL;
}

int main(int argc, char **argv) {
 /* line-buffered output into pipes and log files, as the launcher reads them while the game runs */
 setvbuf(stdout, NULL, _IOLBF, 0);
 /* AC_DEBUG_PTRACE=1: let a debugger or eu-stack attach although it is not this process's parent (Yama
  * ptrace_scope 1, the default on most distributions), for thread stacks while the game runs */
 const char *ptrace_env = getenv("AC_DEBUG_PTRACE");
 if (ptrace_env && ptrace_env[0] == '1') prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
 saved_argc = argc;
 saved_argv = argv;
 pthread_attr_t attr;
 pthread_attr_init(&attr);
 pthread_attr_setstacksize(&attr, 64u << 20);
 pthread_t t;
 if (pthread_create(&t, &attr, run, NULL)) { perror("main thread"); return 1; }
 pthread_attr_destroy(&attr);
 pthread_join(t, NULL);
 /* main ended through ExitThread (a guest thread exiting in parallel mode): as on Windows, the process lives on
  * until its last thread ends */
 if (!returned) pthread_exit(NULL);
 fflush(stdout);
 fflush(stderr);
 return exit_code;
}
