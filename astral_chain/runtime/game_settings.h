/* Settings of the in-game menu (las/menu.cpp): AC_* and LARECOMP_* values in a file of NAME=value lines,
 * AC_SETTINGS_FILE (the launcher points it at settings.ini in the install folder; without it nothing is read or
 * saved). An environment variable set before the start wins over the file and is shown fixed in the menu (tests,
 * a variable given by hand).
 *
 * settings_load (the start of main, before any thread) puts the file's values into the environment, so a setting read
 * once at the start (getenv, GetEnvironmentVariableA) needs nothing more. A setting the menu changes while the game
 * runs is read through setting_get / setting_int by its reader, which reads it again when settings_version changes
 * (the environment is not written after the start: other threads read it). settings_save writes every value. */
#ifdef __linux__
#include <sys/prctl.h>
#endif
#define SETTINGS_MAX 64
typedef struct { char name[40]; char value[64]; unsigned char fixed,unset; } Setting;  /* (unset: not written) */
static Setting settings_tab[SETTINGS_MAX];
static unsigned settings_count;
static SRWLOCK settings_lock=SRWLOCK_INIT;
static volatile LONG settings_changes;
static char settings_path[MAX_PATH];

static Setting *settings_find(const char *name) {
 for(unsigned i=0;i<settings_count;i++) if(!strcmp(settings_tab[i].name,name)) return &settings_tab[i];
 return NULL;
}
static Setting *settings_add(const char *name) {
 Setting *s=settings_find(name);
 if(s || settings_count==SETTINGS_MAX || strlen(name)>=sizeof s->name) return s;
 s=&settings_tab[settings_count++];
 memset(s,0,sizeof *s);
 strcpy(s->name,name);
 return s;
}
static void settings_env_put(const char *name,const char *value) {
#ifdef _WIN32
 _putenv_s(name,value);  /* (getenv reads the C runtime's copy) */
#endif
 SetEnvironmentVariableA(name,value);
}

void settings_load(void) {
 DWORD n=GetEnvironmentVariableA("AC_SETTINGS_FILE",settings_path,sizeof settings_path);
 if(!n || n>=sizeof settings_path) {settings_path[0]=0;return;}
 FILE *f=fopen(settings_path,"r");
 if(!f) return;
 char line[256];
 unsigned loaded=0,fixed=0;
 while(fgets(line,sizeof line,f)) {
  char *p=line;
  while(*p==' ' || *p=='\t') p++;
  char *eq=strchr(p,'=');
  if(*p=='#' || !eq) continue;
  *eq=0;
  char *value=eq+1,*end=value+strlen(value);
  while(end>value && (end[-1]=='\n' || end[-1]=='\r' || end[-1]==' ')) *--end=0;
  for(char *e=eq;e>p && (e[-1]==' ' || e[-1]=='\t');) *--e=0;
  if((strncmp(p,"AC_",3) && strncmp(p,"LARECOMP_",9)) || !strcmp(p,"AC_SETTINGS_FILE")) continue;
  Setting *s=settings_add(p);
  if(!s || strlen(value)>=sizeof s->value) continue;
  const char *env=getenv(p);
  if(env) {  /* (given by hand: wins, and the menu shows it fixed) */
   snprintf(s->value,sizeof s->value,"%s",env);s->fixed=1;fixed++;
  } else {
   strcpy(s->value,value);settings_env_put(p,value);
  }
  loaded++;
 }
 fclose(f);
 printf("[settings] %u from %s (%u fixed by the environment)\n",loaded,settings_path,fixed);
#ifdef __linux__
 /* (AC_DEBUG_PTRACE=1 in the file too: a profiler or eu-stack may attach although not this process's parent; the
  * environment's is handled in posix/w32_main.c) */
 const char *ptrace_env=getenv("AC_DEBUG_PTRACE");
 if(ptrace_env && ptrace_env[0]=='1') prctl(PR_SET_PTRACER,PR_SET_PTRACER_ANY,0,0,0);
#endif
}

/* The value (the file's or the menu's, else the environment's) into buf; 0 when there is none. Any thread. */
int setting_get(const char *name,char *buf,unsigned size) {
 int found=0;
 AcquireSRWLockExclusive(&settings_lock);
 Setting *s=settings_find(name);
 if(s && !s->unset) {snprintf(buf,size,"%s",s->value);found=1;}
 ReleaseSRWLockExclusive(&settings_lock);
 if(!found && !s) {
  const char *env=getenv(name);
  if(env) {snprintf(buf,size,"%s",env);found=1;}
 }
 return found;
}
int setting_int(const char *name,int otherwise) {
 char v[64];
 return setting_get(name,v,sizeof v) && v[0] ? atoi(v) : otherwise;
}
/* Set by the environment at the start: the menu cannot change it. */
int setting_fixed(const char *name) {
 AcquireSRWLockExclusive(&settings_lock);
 Setting *s=settings_find(name);
 int fixed=s ? s->fixed : getenv(name)!=NULL && settings_path[0];
 ReleaseSRWLockExclusive(&settings_lock);
 return fixed;
}
void setting_set(const char *name,const char *value) {
 AcquireSRWLockExclusive(&settings_lock);
 Setting *s=settings_find(name);
 if(!s && getenv(name)) {s=settings_add(name);if(s) {snprintf(s->value,sizeof s->value,"%s",getenv(name));s->fixed=1;}}
 else if(!s) s=settings_add(name);
 int changed=0;
 if(s && !s->fixed && !value) {  /* (NULL: not set at all, for a setting read by its presence) */
  changed=!s->unset;s->unset=1;s->value[0]=0;
 } else if(s && !s->fixed && (s->unset || strcmp(s->value,value))) {
  snprintf(s->value,sizeof s->value,"%s",value);s->unset=0;changed=1;
 }
 ReleaseSRWLockExclusive(&settings_lock);
 if(changed) {InterlockedIncrement(&settings_changes);printf("[settings] %s=%s\n",name,value ? value : "(unset)");}
}
/* Changes since the start: a reader of a live setting reads it again when this moved. */
unsigned settings_version(void) {return (unsigned)settings_changes;}
/* var = the integer setting (or otherwise), read again only after a change */
#define SETTING_LIVE(var,name,otherwise) do {static unsigned version_=~0u; \
 if(version_!=settings_version()) {version_=settings_version();var=setting_int(name,otherwise);}} while(0)
/* Writes every value (a fixed one as the file had it is lost: its environment value is written). 1 when written. */
int settings_save(void) {
 if(!settings_path[0]) return 0;
 char tmp[MAX_PATH+8];
 snprintf(tmp,sizeof tmp,"%s.tmp",settings_path);
 FILE *f=fopen(tmp,"w");
 if(!f) return 0;
 fputs("# Astral Chain PC settings: written by the in-game menu (F1, or Minus and Plus together)\n",f);
 AcquireSRWLockExclusive(&settings_lock);
 for(unsigned i=0;i<settings_count;i++) if(!settings_tab[i].unset) fprintf(f,"%s=%s\n",settings_tab[i].name,settings_tab[i].value);
 ReleaseSRWLockExclusive(&settings_lock);
 int ok=!ferror(f);
 if(fclose(f)) ok=0;
 if(ok) ok=MoveFileExA(tmp,settings_path,MOVEFILE_REPLACE_EXISTING)!=0;
 printf("[settings] %s %s\n",ok ? "saved to" : "could not save",settings_path);
 return ok;
}
