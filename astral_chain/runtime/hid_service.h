/* Process-local HID with a keyboard-backed FullKey controller in slot 0.
 * Publication runs on the guest thread; no asynchronous guest-memory writer. */
#define HID_MEMORY_SIZE 0x40000
static HANDLE hid_memory;
static uint8_t *hid_page;
static unsigned hid_active,hid_revision,hid_styles,hid_ids=1;
/* Handheld attachment policy is retained even though the desktop backend has
 * no handheld device. It does not change the FullKey controller's connection. */
static uint64_t hid_handheld_mode;
static uint64_t hid_joy_hold_type;
/* Held keys by Windows virtual-key code. Mouse buttons and wheel use their codes too (las/input_bindings.h); they are
 * only set while the game has the mouse (mouse.h). */
static uint8_t hid_keys[256];
/* Keyboard and mouse mapping (the launcher's Controls): two bindings per action, las/input_bindings.h. */
#include "las/input_bindings.h"
static uint8_t hid_key_map[2][IB_ACTIONS];
static uint8_t hid_key_mapped[256];
static unsigned hid_mouse_bound;  /* (a mouse button or the wheel is bound: clicking the window takes the mouse) */
static void hid_keys_load(void) {
 static int loaded;
 if(loaded) return;
 loaded=1;
 int custom=ib_load(hid_key_map);
 for(unsigned s=0;s<2;s++) for(unsigned i=0;i<IB_ACTIONS;i++) {
  unsigned vk=hid_key_map[s][i];
  if(vk) hid_key_mapped[vk]=1;
  if(vk && ib_is_mouse(vk)) hid_mouse_bound=1;
 }
 if(custom) printf("[input] key mapping from AC_KEYS / AC_KEYS2\n");
}
static int hid_key(unsigned i) {
 return (hid_key_map[0][i] && hid_keys[hid_key_map[0][i]]) || (hid_key_map[1][i] && hid_keys[hid_key_map[1][i]]);
}
static uint64_t hid_last_tick,hid_sampling;
static unsigned hid_touch_active;
static uint64_t hid_touch_last_tick,hid_touch_sampling;
#include "keyboard.h"
#include "mouse.h"
static HANDLE hid_style_events[10];
static uint64_t hid_sensor_started[2];
static unsigned hid_sensor_index(uint32_t handle) {
 unsigned style=handle&255,player=(handle>>8)&255,device=(handle>>16)&255;
 if(handle>>24 || style<3 || style>7 || (player>7 && player!=0x20) ||
    (style<=4 ? device!=2 : style==5 ? device>1 : style==6 ? device!=0 : device!=1))
  boundary("unsupported-sixaxis-sensor-handle",&state,state.pc);
 unsigned slot=player==0x20 ? 8 : player;
 return ((style-3)*9+slot)*2+(device==1);
}
/* Subscription keys for SDK FullKey/Handheld/Joy-Con device handles. A
 * registration may precede a connection; it does not create physical hardware. */
static unsigned hid_vibration_index(uint32_t handle) {
 unsigned style=handle&255,player=(handle>>8)&255,device=(handle>>16)&255;
 if(handle>>24 || style<3 || style>7 || device>1 || (player>7 && player!=0x20) ||
    (style==6 && device!=0) || (style==7 && device!=1))
  boundary("unsupported-vibration-device-handle",&state,state.pc);
 unsigned slot=player==0x20 ? 8 : player;
 return ((style-3)*9+slot)*2+device;
}
typedef struct { uint64_t sample,buttons; int32_t lx,ly,rx,ry; uint32_t attributes,reserved; } HidSample;
static void hid_rumble_set(unsigned device,float low,float high);  /* (below: the pads' motors) */
static void hid_accept_vibration(const IpcRequest *request) {
 if(!request->send_pid || request->argument_size<32 || request->argument_size>40 ||
    request->send_count || request->receive_count || request->copy_count || request->input_object_count)
  boundary("hid-vibration-layout",&state,state.pc);
 uint64_t aruid;memcpy(&aruid,request->arguments+24,8);
 if(!hid_page || aruid!=GetCurrentProcessId()) boundary("invalid-hid-resource-user",&state,state.pc);
 unsigned handle=ipc_u32(request->arguments);
 float value[4];memcpy(value,request->arguments+4,sizeof(value));
 /* Values are not checked further: there is no actuator to drive, and the console clamps rather than rejects. A
  * negative or non-finite value used to stop the game here (an explosion in the File 01 boss fight, 2026-10-21). */
 static unsigned odd_logged;
 for(unsigned i=0;i<4;i++) {
  unsigned bits=ipc_u32(request->arguments+4+4*i);
  if(((bits&0x7f800000u)==0x7f800000u || value[i]<0) && odd_logged<10) {
   odd_logged++;
   printf("HID vibration handle=0x%x value %u out of range (bits 0x%08x): ignored\n",handle,i,bits);
  }
 }
 HOT_LOG("HID vibration handle=0x%x low=%g/%g high=%g/%g\n",handle,value[0],value[1],value[2],value[3]);
 hid_rumble_set((handle>>16)&1,value[0],value[2]);
}
typedef struct { uint64_t sequence; HidSample state; } HidSampleSlot;
typedef struct { uint64_t unused,capacity,tail,count; HidSampleSlot slots[17]; } HidRing;
_Static_assert(sizeof(HidSample)==0x28 && sizeof(HidRing)==0x350,"HID ABI");
typedef struct { uint64_t sample; uint32_t count,reserved; uint8_t touches[16][40]; } HidTouchSample;
typedef struct { uint64_t sequence; HidTouchSample state; } HidTouchSlot;
typedef struct { uint64_t unused,capacity,tail,count; HidTouchSlot slots[17]; } HidTouchRing;
_Static_assert(sizeof(HidTouchSample)==0x290 && sizeof(HidTouchRing)==0x2c38,"touch HID ABI");
static void hid_touch_publish(uint64_t now) {
 if(!hid_page || !hid_touch_active) return;
 HidTouchRing *ring=(HidTouchRing*)(hid_page+0x400);
 unsigned next=(unsigned)((ring->tail+1)%17);
 /* This desktop backend has no touch device. Publish current no-contact
  * samples, not synthetic finger presses or stale touchscreen data. */
 HidTouchSample sample={0};sample.sample=++hid_touch_sampling;
 ring->slots[next].state=sample;
 InterlockedExchange64((volatile LONG64*)&ring->slots[next].sequence,(LONG64)sample.sample);
 ring->capacity=17;InterlockedExchange64((volatile LONG64*)&ring->tail,next);
 if(ring->count<16) ring->count++;
 hid_touch_last_tick=now;
}
static unsigned hid_slot(unsigned id) {
 if(id<8) return id;
 if(id==0x20) return 8;
 if(id==0x10) return 9;
 boundary("invalid-npad-id",&state,state.pc);
}
static void hid_ring_write(HidRing *ring,const HidSample *sample) {
 unsigned next=(unsigned)((ring->tail+1)%17);
 ring->slots[next].state=*sample;
 InterlockedExchange64((volatile LONG64*)&ring->slots[next].sequence,(LONG64)sample->sample);
 ring->capacity=17;
 InterlockedExchange64((volatile LONG64*)&ring->tail,next);
 if(ring->count<16) ring->count++;
}
/* First connected XInput pad, merged with the keyboard. Positional mapping to
 * the Switch layout: Xbox A/B/X/Y (bottom/right/left/top) -> Switch B/A/Y/X. */
typedef struct {uint16_t buttons;uint8_t lt,rt;int16_t lx,ly,rx,ry;} HidXinputPad;
typedef struct {uint32_t packet;HidXinputPad pad;} HidXinputState;
/* Every connected XInput pad, merged (buttons of any, each stick from the pad that moves it most): a pad that was
 * connected first need not be the one in use. A Steam Controller's receiver is a pad from the start, even with the
 * controller off or its input taken by Steam Input, whose virtual pad comes later; a pad plugged in after the start
 * also comes later. Reading only the first pad found, the game kept reading the receiver until it was restarted.
 * Probing absent pads is slow (~100 ms on a second PC; it ran once per second on the game thread under the kernel
 * lock, stopping every guest thread, the sound's too): a background thread looks for pads each second, the game
 * thread reads the ones found. */
static DWORD (WINAPI *xinput_get_state)(DWORD,HidXinputState*);
static volatile LONG xinput_pads;  /* bit i: pad i answered */
static DWORD WINAPI xinput_probe(void *argument) {
 (void)argument;
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
 for(;;) {
  for(int i=0;i<4;i++) {
   if(xinput_pads&(1<<i)) continue;
   HidXinputState s;
   if(!xinput_get_state(i,&s)) {InterlockedOr(&xinput_pads,1<<i);printf("HID pad %d connected\n",i);fflush(stdout);}
  }
  Sleep(1000);
 }
 return 0;
}
/* Vibration: the game sends HD Rumble values for each half of the controller (the device byte of the handle: left,
 * right), a low band (~160 Hz, a thump) and a high band (~320 Hz, a buzz), each an amplitude of about 0 to 1 and a
 * frequency. A pad has a heavy motor on the left and a light one on the right: the low bands of both halves drive the
 * left motor, the high bands the right one, on every connected pad (XInputSetState: SDL_RumbleGamepad on Linux, which
 * the Xbox, PlayStation and Steam controllers and the Steam Deck support). The motors are weaker than the Switch's
 * actuators at small amplitudes, so the amplitude's square root drives them. A value lasts until the game sends
 * another, and SDL stops a motor after a second: hid_xinput sends it again every half second while it is not zero.
 * AC_VIBRATION=<0..200>: the strength in percent (the in-game menu; 0: off). */
static DWORD (WINAPI *xinput_set_state)(DWORD,uint16_t*);
static float hid_rumble_amp[2][2];  /* [device][low, high] */
static uint16_t hid_rumble_sent[2];
static uint64_t hid_rumble_tick;
static int hid_rumble_strength=100;
static void hid_rumble_send(int force) {
 SETTING_LIVE(hid_rumble_strength,"AC_VIBRATION",100);
 float k=hid_rumble_strength<0 ? 0.0f : hid_rumble_strength/100.0f;
 uint16_t motor[2];
 for(unsigned band=0;band<2;band++) {
  float a=hid_rumble_amp[0][band]>hid_rumble_amp[1][band] ? hid_rumble_amp[0][band] : hid_rumble_amp[1][band];
  float v=a>0 ? sqrtf(a)*k : 0.0f;
  motor[band]=v>=1.0f ? 65535 : (uint16_t)(v*65535.0f);
 }
 uint64_t now=GetTickCount64();
 int changed=motor[0]!=hid_rumble_sent[0] || motor[1]!=hid_rumble_sent[1];
 if(!changed && !(force && (motor[0] || motor[1]) && now-hid_rumble_tick>=500)) return;
 hid_rumble_sent[0]=motor[0];hid_rumble_sent[1]=motor[1];hid_rumble_tick=now;
 if(!xinput_set_state) return;
 LONG pads=xinput_pads;
 for(int pad=0;pad<4;pad++) if(pads&(1<<pad)) {uint16_t m[2]={motor[0],motor[1]};xinput_set_state((DWORD)pad,m);}
}
static void hid_rumble_set(unsigned device,float low,float high) {
 /* (finite and not negative: a bad value was seen in the File 01 boss fight; the console clamps) */
 hid_rumble_amp[device][0]=low>0 && low<4 ? low : 0;
 hid_rumble_amp[device][1]=high>0 && high<4 ? high : 0;
 hid_rumble_send(0);
}
static void hid_xinput(HidSample *sample) {
 static int loaded;
 if(!loaded) {
  loaded=1;HMODULE x=LoadLibraryExW(L"xinput1_4.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(x) xinput_get_state=(DWORD (WINAPI*)(DWORD,HidXinputState*))GetProcAddress(x,"XInputGetState");
  if(x) xinput_set_state=(DWORD (WINAPI*)(DWORD,uint16_t*))GetProcAddress(x,"XInputSetState");
  if(xinput_get_state) {HANDLE t=CreateThread(NULL,0,xinput_probe,NULL,0,NULL);if(t) CloseHandle(t);}
 }
 if(!xinput_get_state) return;
 hid_rumble_send(1);  /* (a vibration lasting longer than SDL's second) */
 static const struct {uint16_t xinput;unsigned bit;} map[]={
  {0x1000,1},{0x2000,0},{0x4000,3},{0x8000,2},{0x0040,4},{0x0080,5},{0x0100,6},{0x0200,7},
  {0x0010,10},{0x0020,11},{0x0004,12},{0x0001,13},{0x0008,14},{0x0002,15}};
 /* Sticks: radial dead zone of 8000 (XInput guidance), then full scale; the keys' direction wins (already set). */
 int32_t *out[2][2]={{&sample->lx,&sample->ly},{&sample->rx,&sample->ry}};
 int keyed[2]={*out[0][0] || *out[0][1],*out[1][0] || *out[1][1]};
 long long best[2]={8000ll*8000,8000ll*8000};
 LONG pads=xinput_pads;
 for(int pad=0;pad<4;pad++) {
  if(!(pads&(1<<pad))) continue;
  HidXinputState s;
  if(xinput_get_state((DWORD)pad,&s)) {InterlockedAnd(&xinput_pads,~(1<<pad));printf("HID pad %d disconnected\n",pad);fflush(stdout);continue;}
  for(unsigned i=0;i<sizeof(map)/sizeof(map[0]);i++) if(s.pad.buttons&map[i].xinput) sample->buttons|=1ull<<map[i].bit;
  if(s.pad.lt>30) sample->buttons|=1ull<<8;
  if(s.pad.rt>30) sample->buttons|=1ull<<9;
  int in[2][2]={{s.pad.lx,s.pad.ly},{s.pad.rx,s.pad.ry}};
  for(unsigned k=0;k<2;k++) {
   long long m=(long long)in[k][0]*in[k][0]+(long long)in[k][1]*in[k][1];
   if(m>best[k] && !keyed[k]) {
    best[k]=m;
    *out[k][0]=in[k][0]<-32767?-32767:in[k][0];*out[k][1]=in[k][1]<-32767?-32767:in[k][1];
   }
  }
 }
}
/* Test automation: AC_INPUT_SCRIPT="t0-t1:mask;..." holds Switch button mask
 * (bit 0 A ... bit 9 ZR, hex) from t0 to t1 seconds after the first sample.
 * Applies regardless of window focus. */
/* Test automation, live: AC_INPUT_FILE=<file> holds the button mask (hex, same bits) the file holds, read every
 * 20 ms; a missing or empty file is no buttons. Applies regardless of window focus. */
static uint64_t hid_file_buttons(uint64_t now) {
 static char path[MAX_PATH];static int loaded;static uint64_t checked,mask;
 if(!loaded) {loaded=1;DWORD n=GetEnvironmentVariableA("AC_INPUT_FILE",path,sizeof(path));if(!n || n>=sizeof(path))path[0]=0;}
 if(!path[0] || now-checked<384000) return mask;
 checked=now;
 unsigned long long m=0;FILE *f=fopen(path,"r");
 if(f) {if(fscanf(f,"%llx",&m)!=1) m=0;fclose(f);}
 if(m!=mask) printf("HID file buttons=0x%llx\n",m);
 mask=m;return mask;
}
static uint64_t hid_script_buttons(uint64_t now) {
 static char script[4096];static int loaded;static uint64_t origin;
 if(!loaded) {loaded=1;DWORD n=GetEnvironmentVariableA("AC_INPUT_SCRIPT",script,sizeof(script));if(!n || n>=sizeof(script))script[0]=0;}
 if(!script[0]) return hid_file_buttons(now);
 if(!origin) origin=now;
 double t=(double)(now-origin)/19200000.0;uint64_t mask=0;
 for(const char *p=script;*p;) {
  double a,b;unsigned long long m;int used=0;
  if(sscanf(p,"%lf-%lf:%llx%n",&a,&b,&m,&used)!=3) break;
  p+=used;
  /* Optional "@period/duty": inside [a,b) held for the first `duty` seconds of every `period`. */
  double period=0,duty=0;int more=0;
  if(*p=='@' && sscanf(p,"@%lf/%lf%n",&period,&duty,&more)==2) p+=more; else period=0;
  if(t>=a && t<b && (period<=0 || fmod(t-a,period)<duty)) mask|=m;
  if(*p==';') p++;
 }
 static uint64_t last=~0ull;
 if(mask!=last) {printf("HID script t=%.2f buttons=0x%llx\n",t,(unsigned long long)mask);last=mask;}
 return mask|hid_file_buttons(now);
}
/* The in-game menu (las/menu.h): weak, a build without the Vulkan layer has none. */
int menu_is_open(void) __attribute__((weak));
void menu_toggle(void) __attribute__((weak));
void menu_key(unsigned vk,int down) __attribute__((weak));
void menu_mouse_move(float x,float y,float client_w,float client_h) __attribute__((weak));
void menu_mouse_button(int button,int down) __attribute__((weak));
void menu_mouse_wheel(float steps) __attribute__((weak));
int menu_pad_gate(uint64_t buttons,int32_t lx,int32_t ly) __attribute__((weak));
static int menu_open(void) {return menu_is_open && menu_is_open();}
static void hid_publish(uint64_t now,int focused) {
 if(!hid_page || !hid_active) return;
 unsigned connected=(hid_styles&1) && (hid_ids&1);
 uint8_t *npad=hid_page+0x9a00;
 unsigned previous=ipc_u32(npad);
 ipc_put32(npad,connected ? 1 : 0);
 ipc_put32(npad+8,connected ? 0 : 2); /* full-key color: OK / no controller */
 ipc_put32(npad+20,2); /* no split Joy-Con colors */
 ipc_put32(npad+0x4188,connected ? 1 : 0);
 ipc_put32(npad+0x419c,connected ? 4 : 0);
 if(previous!=(connected ? 1u : 0u) && hid_style_events[0] && !SetEvent(hid_style_events[0]))
  boundary("hid-style-event",&state,state.pc);
 HidSample sample={0};sample.sample=++hid_sampling;sample.attributes=connected ? 3 : 0;
 int gated=0;uint64_t raw=0;
 mouse_wheel_step(now);
 if(connected && focused) {
  hid_keys_load();
  for(unsigned i=0;i<16;i++) if(hid_key(i)) sample.buttons|=1ull<<i;
  sample.lx=(hid_key(18)-hid_key(16))*32767;sample.ly=(hid_key(17)-hid_key(19))*32767;
  sample.rx=(hid_key(22)-hid_key(20))*32767;sample.ry=(hid_key(21)-hid_key(23))*32767;
  if(sample.lx && sample.ly) {sample.lx=sample.lx>0 ? 23170 : -23170;sample.ly=sample.ly>0 ? 23170 : -23170;}
  if(sample.rx && sample.ry) {sample.rx=sample.rx>0 ? 23170 : -23170;sample.ry=sample.ry>0 ? 23170 : -23170;}
  hid_xinput(&sample);
  mouse_look_stick(&sample.rx,&sample.ry,now);
  if(sample.lx<0) sample.buttons|=1ull<<16;
  if(sample.ly>0) sample.buttons|=1ull<<17;
  if(sample.lx>0) sample.buttons|=1ull<<18;
  if(sample.ly<0) sample.buttons|=1ull<<19;
  if(sample.rx<0) sample.buttons|=1ull<<20;
  if(sample.ry>0) sample.buttons|=1ull<<21;
  if(sample.rx>0) sample.buttons|=1ull<<22;
  if(sample.ry<0) sample.buttons|=1ull<<23;
 }
 if(connected) {
  uint64_t scripted=hid_script_buttons(now);sample.buttons|=scripted;
  /* Scripted stick directions (bits 16-23) also drive the analog axes. */
  if(scripted&(1ull<<16)) sample.lx=-32767;
  if(scripted&(1ull<<18)) sample.lx=32767;
  if(scripted&(1ull<<17)) sample.ly=32767;
  if(scripted&(1ull<<19)) sample.ly=-32767;
  /* (Minus and Plus together open the menu; while it is open, and until what was held is let go, no game input) */
  gated=menu_pad_gate && menu_pad_gate(sample.buttons,sample.lx,sample.ly);
  raw=sample.buttons;
  if(gated) {sample.buttons=0;sample.lx=sample.ly=sample.rx=sample.ry=0;}
 }
 /* AC_INPUT_LOG=1: each change of the buttons the game gets (hex, bit 8 ZL, bit 9 ZR), with the time and why input
  * was held back (window not focused: no pad or keyboard input; the in-game menu open or its buttons still held). */
 {
  static int on=-1;static uint64_t last=~0ull,origin;static int last_focus=-1,last_gated=-1;
  if(on<0) {char v[8];DWORD n=GetEnvironmentVariableA("AC_INPUT_LOG",v,sizeof(v));on=n && n<sizeof(v) && atoi(v);}
  if(on && (sample.buttons!=last || focused!=last_focus || gated!=last_gated)) {
   if(!origin) origin=now;
   printf("HID input t=%.3f buttons=0x%llx",(double)(now-origin)/19200000.0,(unsigned long long)sample.buttons);
   if(!focused) printf(" (window not focused)");
   if(gated) printf(" (held back by the menu; pressed 0x%llx)",(unsigned long long)raw);
   printf("\n");fflush(stdout);
   last=sample.buttons;last_focus=focused;last_gated=gated;
  }
 }
 hid_ring_write((HidRing*)(npad+0x28),&sample);
 hid_ring_write((HidRing*)(npad+0x1408),&sample);
 hid_last_tick=now;
}
static void hid_poll(int focused) {
 if(!hid_active && !hid_touch_active && !keyboard_active && !mouse_active) return;
 uint64_t now=rt_ticks();
 if(hid_active && (!hid_sampling || now-hid_last_tick>=153600)) hid_publish(now,focused); /* up to 125 Hz */
 if(hid_touch_active && (!hid_touch_sampling || now-hid_touch_last_tick>=153600)) hid_touch_publish(now);
 if(keyboard_active && (!keyboard_sample || now-keyboard_tick>=153600)) keyboard_publish(now,focused);
 if(mouse_active && (!mouse_sample || now-mouse_tick>=153600)) mouse_publish(now,focused);
}
static void hid_initialize(void) {
 if(hid_page) return;
 hid_memory=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,HID_MEMORY_SIZE,NULL);
 if(!hid_memory) boundary("host-hid-memory-create",&state,state.pc);
 hid_page=MapViewOfFile(hid_memory,FILE_MAP_ALL_ACCESS,0,0,HID_MEMORY_SIZE);
 if(!hid_page) boundary("host-hid-memory-view",&state,state.pc);
 /* Anonymous section pages are initially zero: no devices or samples yet. */
}
