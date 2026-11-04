/* Linux host: waveOut on SDL3 audio streams (win32_posix.c). LoadLibrary(L"winmm.dll") resolves these too. */
#pragma once
#include <windows.h>
typedef UINT MMRESULT;
typedef HANDLE HWAVEOUT, *LPHWAVEOUT;
#define MMSYSERR_NOERROR 0u
#define MMSYSERR_ERROR 1u
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_NOTENABLED 3u
#define MMSYSERR_ALLOCATED 4u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_NODRIVER 6u
#define MMSYSERR_NOMEM 7u
#define MMSYSERR_NOTSUPPORTED 8u
#define MMSYSERR_INVALFLAG 10u
#define MMSYSERR_INVALPARAM 11u
#define WAVERR_BADFORMAT 32u
#define WAVERR_STILLPLAYING 33u
#define WAVERR_UNPREPARED 34u
#define WAVE_MAPPER ((UINT)-1)
#define WAVE_FORMAT_PCM 1
#define CALLBACK_NULL 0u
#define CALLBACK_EVENT 0x50000u
#define CALLBACK_FUNCTION 0x30000u
#define WOM_OPEN 0x3BBu
#define WOM_CLOSE 0x3BCu
#define WOM_DONE 0x3BDu
#define WHDR_DONE 1u
#define WHDR_PREPARED 2u
#define WHDR_BEGINLOOP 4u
#define WHDR_ENDLOOP 8u
#define WHDR_INQUEUE 16u
#define TIME_MS 1u
#define TIME_SAMPLES 2u
#define TIME_BYTES 4u
#define TIMERR_NOERROR 0u
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; }
 WAVEFORMATEX, *LPWAVEFORMATEX;
typedef const WAVEFORMATEX *LPCWAVEFORMATEX;
typedef struct wavehdr_tag { LPSTR lpData; DWORD dwBufferLength, dwBytesRecorded; DWORD_PTR dwUser; DWORD dwFlags, dwLoops;
 struct wavehdr_tag *lpNext; DWORD_PTR reserved; } WAVEHDR, *LPWAVEHDR;
typedef struct { UINT wType; union { DWORD ms, sample, cb, ticks; struct { BYTE hour, min, sec, frame, fps, dummy, pad[2]; } smpte;
 struct { DWORD songptrpos; } midi; } u; } MMTIME, *LPMMTIME;
MMRESULT waveOutOpen(LPHWAVEOUT out, UINT device, LPCWAVEFORMATEX format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags);
MMRESULT waveOutClose(HWAVEOUT out);
MMRESULT waveOutPrepareHeader(HWAVEOUT out, LPWAVEHDR header, UINT size);
MMRESULT waveOutUnprepareHeader(HWAVEOUT out, LPWAVEHDR header, UINT size);
MMRESULT waveOutWrite(HWAVEOUT out, LPWAVEHDR header, UINT size);
MMRESULT waveOutPause(HWAVEOUT out);
MMRESULT waveOutRestart(HWAVEOUT out);
MMRESULT waveOutReset(HWAVEOUT out);
MMRESULT waveOutGetPosition(HWAVEOUT out, LPMMTIME time, UINT size);
MMRESULT waveOutSetVolume(HWAVEOUT out, DWORD volume);
MMRESULT timeBeginPeriod(UINT period);
MMRESULT timeEndPeriod(UINT period);
