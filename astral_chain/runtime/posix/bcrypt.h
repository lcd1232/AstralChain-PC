/* Linux host: random numbers from getrandom(2) and SHA-256 (win32_posix.c). */
#pragma once
#include <windows.h>
typedef PVOID BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE;
#define BCRYPT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 2u
#define BCRYPT_SHA256_ALGORITHM L"SHA256"
#define BCRYPT_RNG_ALGORITHM L"RNG"
NTSTATUS BCryptGenRandom(BCRYPT_ALG_HANDLE algorithm, PUCHAR buffer, ULONG size, ULONG flags);
NTSTATUS BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *algorithm, LPCWSTR id, LPCWSTR implementation, ULONG flags);
NTSTATUS BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE algorithm, ULONG flags);
NTSTATUS BCryptHash(BCRYPT_ALG_HANDLE algorithm, PUCHAR secret, ULONG secret_size, PUCHAR input, ULONG input_size,
                    PUCHAR output, ULONG output_size);
