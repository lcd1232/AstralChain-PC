/* Linux host: the MSVC intrinsics the runtime uses. */
#pragma once
#include <x86intrin.h>
#include <cpuid.h>
static inline void win32_posix_cpuid(int info[4], int leaf) { __cpuid_count(leaf, 0, info[0], info[1], info[2], info[3]); }
static inline void win32_posix_cpuidex(int info[4], int leaf, int sub) { __cpuid_count(leaf, sub, info[0], info[1], info[2], info[3]); }
#undef __cpuid
#define __cpuid(info, leaf) win32_posix_cpuid((info), (leaf))
#define __cpuidex(info, leaf, sub) win32_posix_cpuidex((info), (leaf), (sub))
