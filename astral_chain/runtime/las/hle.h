/* Astral Chain shim for LASRecomp's runtime/hle.h (see README.md).
 * HLE_FUNC entries live in COFF section .hle$m, bracketed by .hle$a/.hle$z
 * markers (on Linux: ELF section ac_nvn_hle); nvn_hle.h looks implementations up by name. */
#pragma once
#include <stdlib.h>
#define ENV(name) ({ static const char *env_v_; static int env_i_; if (!env_i_) { env_v_ = getenv(name); env_i_ = 1; } env_v_; })
#include "rt.h"
typedef struct { const char *name; HostFn fn; } HleImplEntry;
#define HLE_CAT2(a, b) a##b
#define HLE_CAT(a, b) HLE_CAT2(a, b)
#ifdef __linux__
/* ELF has no COFF '$' grouping: one section with a C-identifier name, bounded by the linker's __start_/__stop_
 * symbols (nvn_hle.h). retain: lld's --gc-sections (Zig's default with optimization) drops it otherwise. */
#define HLE_SECTION "ac_nvn_hle"
#define HLE_KEEP used, retain
#else
#define HLE_SECTION ".hle$m"
#define HLE_KEEP used
#endif
#define HLE_FUNC(sym, fn)                                                                   \
    static void fn(Cpu *c);                                                                 \
    __attribute__((HLE_KEEP, section(HLE_SECTION))) static const HleImplEntry               \
        HLE_CAT(hle_entry_, __LINE__) = {sym, fn};                                          \
    static void fn(Cpu *c)
/* No in-place reboot in this runtime: reset hooks are compiled but never run. */
typedef void (*ResetFn)(void);
#define RT_ON_RESET(fn) __attribute__((unused)) static void fn(void)
#define RES_OK 0u
#define MAKE_RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
