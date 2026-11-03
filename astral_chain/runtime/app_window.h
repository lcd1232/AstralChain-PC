/* Native application lifecycle. Desktop input focus is distinct from the
 * guest applet's active state: minimize backgrounds it; ordinary alt-tab does not. */
static HWND app_window;
static uint8_t *app_frame_pixels;
static unsigned app_frame_width,app_frame_height;
static uint64_t app_frame_tick,app_frame_count;
static int (WINAPI *app_stretch_dib)(HDC,int,int,int,int,int,int,int,int,const void *,const BITMAPINFO *,UINT,DWORD);
static void app_paint_frame(HDC dc,const RECT *client) {
 if(!app_frame_pixels) return;
 BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
 info.bmiHeader.biWidth=app_frame_width;info.bmiHeader.biHeight=-(LONG)app_frame_height;
 info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
 if(app_stretch_dib(dc,0,0,client->right-client->left,client->bottom-client->top,0,0,
     app_frame_width,app_frame_height,app_frame_pixels,&info,DIB_RGB_COLORS,SRCCOPY)==GDI_ERROR)
  boundary("host-display-paint",&state,state.pc);
}
static void app_snapshot_frame(const char *path) {
 if(!app_frame_pixels) return;
 char name[1024];if(snprintf(name,sizeof(name),"%s.frame.bmp",path)>=(int)sizeof(name)) {fputs("Display snapshot path too long\n",stderr);ExitProcess(33);}
 FILE *file=fopen(name,"wb");if(!file) {fputs("Display snapshot open failed\n",stderr);ExitProcess(33);}
 BITMAPFILEHEADER file_header={0};BITMAPINFOHEADER info={0};
 unsigned bytes=app_frame_width*app_frame_height*4;
 file_header.bfType=0x4d42;file_header.bfOffBits=sizeof(file_header)+sizeof(info);file_header.bfSize=file_header.bfOffBits+bytes;
 info.biSize=sizeof(info);info.biWidth=app_frame_width;info.biHeight=-(LONG)app_frame_height;
 info.biPlanes=1;info.biBitCount=32;info.biSizeImage=bytes;
 int ok=fwrite(&file_header,1,sizeof(file_header),file)==sizeof(file_header) && fwrite(&info,1,sizeof(info),file)==sizeof(info) && fwrite(app_frame_pixels,1,bytes,file)==bytes;
 if(fclose(file) || !ok) {fputs("Display snapshot write failed\n",stderr);ExitProcess(33);}
}
static int app_input_focus;
static int app_foreground_owned;
static int app_resume_notifications;
static int app_operation_notifications,app_performance_notifications;
/* Desktop compatibility profile; independent of host CPU/GPU clock controls. */
static unsigned app_operation_mode=1,app_performance_mode=PERFORMANCE_MODE;
static int app_focus_policy, app_suspend_background, app_suspend_out_of_focus, app_exit_requested;
static uint64_t app_suspended_ticks;
static unsigned app_focus=3, app_messages[64], app_message_head, app_message_count;
static void app_enqueue(unsigned message) {
 if(app_message_count==64) boundary("applet-message-overflow",&state,state.pc);
 app_messages[(app_message_head+app_message_count++)%64]=message;
 if(!SetEvent(app_message_event)) boundary("applet-event-signal",&state,state.pc);
}
static void app_set_focus(unsigned focus) {
 if(app_focus!=focus) {app_focus=focus;app_enqueue(15);printf("WINDOW focus=%u\n",focus);fflush(stdout);}
}
/* AC_HANDHELD=1: the game is told it runs handheld (operation mode 0, normal performance mode), as on a Switch out of
 * its dock: it then renders at its handheld resolution, 1280x720, instead of the docked 1600x900. The in-game menu
 * changes it while the game runs (nvn_hle.h nvn_present_pace: app_set_modes, the dock's messages). */
static void app_modes_load(void) {
 static int done;char v[4];
 if(done) return;
 done=1;
 if(GetEnvironmentVariableA("AC_HANDHELD",v,sizeof v)==1 && v[0]=='1') {
  app_operation_mode=0;app_performance_mode=0;
  printf("[app] handheld mode (AC_HANDHELD=1)\n");fflush(stdout);
 }
}
static void app_set_modes(unsigned operation,unsigned performance) {
 if(operation>1 || performance>1) boundary("invalid-application-mode",&state,state.pc);
 if(app_operation_mode!=operation) {
  app_operation_mode=operation;if(app_operation_notifications) app_enqueue(30);
 }
 if(app_performance_mode!=performance) {
  app_performance_mode=performance;if(app_performance_notifications) app_enqueue(31);
 }
}
static int app_receive(unsigned *message) {
 if(!app_message_count) return 0;
 *message=app_messages[app_message_head];app_message_head=(app_message_head+1)%64;
 if(!--app_message_count && !ResetEvent(app_message_event)) boundary("applet-event-reset",&state,state.pc);
 return 1;
}
static volatile ULONGLONG app_save_write_tick;  /* (save_fs.h) */
static DWORD WINAPI app_exit_watchdog(LPVOID unused) {
 (void)unused;Sleep(1500);
 /* A save being written is not cut off (a write goes straight into the save file): wait until writes have been quiet
  * for 1 s, at most 5 s more. */
 for(int i=0;i<50 && GetTickCount64()-app_save_write_tick<1000;i++) Sleep(100);
 fflush(stdout);fflush(stderr);TerminateProcess(GetCurrentProcess(),0);return 0;
}
/* Fullscreen: a borderless window covering its monitor (no display mode change; the swapchain follows the window
 * size and the picture keeps its aspect ratio, device.c). F11 or Alt+Enter toggles it (unless the key is a game key in
 * the launcher's Controls); AC_FULLSCREEN=1 starts in it (nvn_host_window). */
static int app_fullscreen;static WINDOWPLACEMENT app_windowed_place={sizeof(WINDOWPLACEMENT)};
static void app_set_fullscreen(HWND window,int on) {
 if(!window || on==app_fullscreen) return;
 if(on) {
  MONITORINFO info={sizeof info};
  if(!GetWindowPlacement(window,&app_windowed_place) || !GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info)) return;
  SetWindowLongPtrW(window,GWL_STYLE,(GetWindowLongPtrW(window,GWL_STYLE)&~WS_OVERLAPPEDWINDOW)|WS_POPUP);
  SetWindowPos(window,HWND_TOP,info.rcMonitor.left,info.rcMonitor.top,info.rcMonitor.right-info.rcMonitor.left,
   info.rcMonitor.bottom-info.rcMonitor.top,SWP_FRAMECHANGED|SWP_NOOWNERZORDER);
 } else {
  SetWindowLongPtrW(window,GWL_STYLE,(GetWindowLongPtrW(window,GWL_STYLE)&~WS_POPUP)|WS_OVERLAPPEDWINDOW);
  SetWindowPlacement(window,&app_windowed_place);
  SetWindowPos(window,NULL,0,0,0,0,SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER);
 }
 app_fullscreen=on;
 printf("[app] %s\n",on ? "fullscreen" : "windowed");
}
/* AC_WINDOW_SIZE=<w>x<h> (the in-game menu's Window size; default 1600x900): the window's client size when not
 * fullscreen, at the start (nvn_host_window) and when the menu changes it. */
static int app_window_size_get(int *w,int *h) {
 char v[32];int x=0,y=0;
 if(!setting_get("AC_WINDOW_SIZE",v,sizeof v) || sscanf(v,"%dx%d",&x,&y)!=2 || x<640 || y<360 || x>7680 || y>4320) {x=1600;y=900;}
 *w=x;*h=y;return 1;
}
static void app_window_size_apply(HWND window) {
 int w,h;app_window_size_get(&w,&h);
 RECT r={0,0,w,h};AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
 SetWindowPos(window,NULL,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
 printf("[app] window %dx%d\n",w,h);
}
static void app_window_size_live(void) {
 static unsigned version=~0u;static char last[32];static int seen,pending;
 if(!app_window) return;
 if(version!=settings_version()) {
  version=settings_version();
  char v[32]="";setting_get("AC_WINDOW_SIZE",v,sizeof v);
  if(seen && strcmp(v,last)) pending=1;   /* (the first look: the start already used it) */
  seen=1;strcpy(last,v);
 }
 if(pending && !app_fullscreen) {pending=0;app_window_size_apply(app_window);}   /* (in fullscreen: when it ends) */
}
/* The cursor over the window hides after 3 s without moving (playing with a controller) and shows again when the mouse
 * moves; not while the menu is open. */
static uint64_t app_mouse_moved;static int app_cursor_idle;
/* What the in-game menu asks (las/menu.h), from the render thread: done by app_pump on the window thread. */
static volatile LONG app_menu_fullscreen=-1,app_menu_quit;
int host_menu_fullscreen(int on) {
 if(on>=0) InterlockedExchange(&app_menu_fullscreen,on);
 LONG pending=app_menu_fullscreen;
 return pending>=0 ? (int)pending : app_fullscreen;
}
void host_menu_quit(void) {InterlockedExchange(&app_menu_quit,1);printf("[app] quit from the menu\n");fflush(stdout);}
static LRESULT CALLBACK app_window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
 /* Dragging by the title bar is done here: Windows' own move loop runs inside this call, on the game's main thread
  * and under the kernel lock, and held the whole game (sound included) for as long as the button was down. */
 static int dragging;static POINT grab;
 if(message==WM_NCLBUTTONDOWN && wparam==HTCAPTION && !IsZoomed(window)) {
  POINT p;RECT r;GetCursorPos(&p);GetWindowRect(window,&r);
  grab.x=p.x-r.left;grab.y=p.y-r.top;dragging=1;SetCapture(window);return 0;
 }
 if(dragging) {
  if(message==WM_MOUSEMOVE) {
   POINT p;GetCursorPos(&p);
   SetWindowPos(window,NULL,p.x-grab.x,p.y-grab.y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);return 0;
  }
  if(message==WM_LBUTTONUP) {dragging=0;ReleaseCapture();return 0;}
  if(message==WM_CAPTURECHANGED) {dragging=0;return 0;}
 }
 if(message==WM_PAINT && app_frame_pixels) {
  PAINTSTRUCT paint;HDC dc=BeginPaint(window,&paint);RECT client;GetClientRect(window,&client);
  app_paint_frame(dc,&client);EndPaint(window,&paint);return 0;
 }
 if(message==WM_MOUSEMOVE || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_RBUTTONDOWN || message==WM_RBUTTONUP ||
    message==WM_MBUTTONDOWN || message==WM_MBUTTONUP || message==WM_XBUTTONDOWN || message==WM_XBUTTONUP || message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) {
  app_mouse_moved=GetTickCount64();
  if(app_cursor_idle) {app_cursor_idle=0;if(!mouse_captured) SetCursor(LoadCursorW(NULL,MAKEINTRESOURCEW(32512)));}
  if(menu_open()) {  /* (the menu has the mouse) */
   RECT client;GetClientRect(window,&client);
   if(message==WM_MOUSEMOVE) menu_mouse_move((float)(short)LOWORD(lparam),(float)(short)HIWORD(lparam),(float)client.right,(float)client.bottom);
   else if(message==WM_LBUTTONDOWN || message==WM_LBUTTONUP) menu_mouse_button(0,message==WM_LBUTTONDOWN);
   else if(message==WM_RBUTTONDOWN || message==WM_RBUTTONUP) menu_mouse_button(1,message==WM_RBUTTONDOWN);
   else if(message==WM_MBUTTONDOWN || message==WM_MBUTTONUP) menu_mouse_button(2,message==WM_MBUTTONDOWN);
   else if(message==WM_MOUSEWHEEL) menu_mouse_wheel((float)(short)HIWORD(wparam)/120.0f);
   return message==WM_XBUTTONDOWN || message==WM_XBUTTONUP ? TRUE : 0;
  }
  if(app_input_focus && app_focus==1 && !app_exit_requested) hid_mouse_event(window,message,wparam,lparam);
  return message==WM_XBUTTONDOWN || message==WM_XBUTTONUP ? TRUE : 0;
 }
 if(message==WM_INPUT) {mouse_raw_input(lparam);return DefWindowProcW(window,message,wparam,lparam);}
 if(message==WM_SETCURSOR && mouse_captured && LOWORD(lparam)==HTCLIENT) {SetCursor(NULL);return TRUE;}
 if(message==WM_MOVE || message==WM_EXITSIZEMOVE) mouse_clip();
 /* (capture taken away with a button down: no game button stays held, and that button presses nothing until let go) */
 if(message==WM_CAPTURECHANGED) {if(mouse_buttons) {unsigned held=mouse_buttons;mouse_clear();mouse_keys_clear();mouse_swallow=held;}return 0;}
 if((message==WM_ACTIVATEAPP && !wparam) || (message==WM_SIZE && wparam==SIZE_MINIMIZED) || message==WM_CLOSE) {
  mouse_clear();if(window && GetCapture()==window) ReleaseCapture();
  mouse_set_captured(window,0);mouse_alt_free=0;
 }
 if(message==WM_ACTIVATEAPP) {
  app_input_focus=wparam!=0;
  if(!app_input_focus) {memset(hid_keys,0,sizeof(hid_keys));memset(keyboard_keys,0,sizeof(keyboard_keys));hid_publish(rt_ticks(),0);keyboard_publish(rt_ticks(),0);}
  return 0;
 }
 if((message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) && wparam<256) {
  hid_keys_load();
  /* F1 (unless a game key): the in-game menu; while it is open, the keys are its */
  if(wparam==VK_F1 && menu_toggle && !hid_key_mapped[VK_F1]) {
   if((message==WM_KEYDOWN || message==WM_SYSKEYDOWN) && !(lparam&(1<<30))) menu_toggle();
   return 0;
  }
  if(menu_open()) {
   menu_key((unsigned)wparam,message==WM_KEYDOWN || message==WM_SYSKEYDOWN);
   return message==WM_SYSKEYDOWN || message==WM_SYSKEYUP ? DefWindowProcW(window,message,wparam,lparam) : 0;  /* (Alt+F4) */
  }
  int alt_enter=wparam==VK_RETURN && (lparam&(1<<29)) && !hid_key_mapped[VK_MENU];
  if((wparam==VK_F11 && !hid_key_mapped[VK_F11]) || alt_enter) {
   if((message==WM_KEYDOWN || message==WM_SYSKEYDOWN) && !(lparam&(1<<30))) app_set_fullscreen(window,!app_fullscreen);
   return 0;
  }
  if(app_input_focus && app_focus==1 && !app_exit_requested) {
   int down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
   hid_keys[wparam]=down;keyboard_event((unsigned)wparam,lparam,down);
   /* (holding Alt gives the mouse back, unless Alt is a game key; letting go takes it again) */
   if(wparam==VK_MENU && !hid_key_mapped[VK_MENU]) {
    if(down && mouse_captured) {mouse_set_captured(window,0);mouse_alt_free=1;}
    else if(!down && mouse_alt_free) {mouse_alt_free=0;mouse_set_captured(window,1);mouse_swallow=mouse_buttons;}  /* (as a click) */
   }
  }
  /* (Alt and F10 open the window menu: not when they are game keys. Alt+F4 still closes. Alt alone frees the mouse
   * instead: letting go of it does not enter the window menu, whose loop would hold the game.) */
  hid_keys_load();
  if(message==WM_SYSKEYUP && wparam==VK_MENU) return 0;
  if((message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) && !((wparam==VK_MENU || wparam==VK_F10) && hid_key_mapped[wparam]))
   return DefWindowProcW(window,message,wparam,lparam);
  return 0;
 }
 if(message==WM_SIZE) {
  mouse_clip();
  app_set_focus(wparam==SIZE_MINIMIZED ? 3 : 1);
  if(app_focus==3) {memset(hid_keys,0,sizeof(hid_keys));memset(keyboard_keys,0,sizeof(keyboard_keys));hid_publish(rt_ticks(),0);keyboard_publish(rt_ticks(),0);}
  return 0;
 }
 if(message==WM_CLOSE) {
  /* The game gets the exit request; if shutdown stalls (GPU work, waits), the
   * process must not linger invisibly holding the save lock and GPU memory. */
  if(!app_exit_requested) {HANDLE t=CreateThread(NULL,0,app_exit_watchdog,NULL,0,NULL);if(t)CloseHandle(t);}
  ShowWindow(window,SW_HIDE);  /* (closing looks immediate; the sound is muted too: wave_output.h) */
  app_exit_requested=1;app_enqueue(4);return 0;
 }
 return DefWindowProcW(window,message,wparam,lparam);
}
/* The host thread that created the window: only it can pump the window's messages (parallel mode). */
static DWORD app_window_thread;
static void app_pump(void) {
 if(!app_window || GetCurrentThreadId()!=app_window_thread) return;
 /* At most once a millisecond (AC_PUMP_INTERVAL_US, 0: every call): it runs at every SVC of the window's thread, many
  * a frame, and each run polled SDL (the display connection, D-Bus for the input method, the pads) under the kernel
  * lock: ~5% of the game's main thread on a laptop, every other guest thread stopped meanwhile. The input read
  * (hid_poll) keeps its own 125 Hz. */
 {
  static int64_t interval=-1;static uint64_t last;
  if(interval<0) {char v[16];DWORD n=GetEnvironmentVariableA("AC_PUMP_INTERVAL_US",v,sizeof(v));interval=n && n<sizeof(v) ? atoll(v) : 1000;if(interval<0) interval=0;}
  uint64_t now=rt_ticks();
  if(last && now-last<(uint64_t)interval*192/10) return;   /* (rt_ticks: 19.2 MHz) */
  last=now;
 }
 MSG message;
 unsigned count=0;
 /* (stall reports) this runs under the kernel lock: a slow message or input read stops every guest thread. Steps over
  * 30 ms are logged ("PUMP"). */
 LARGE_INTEGER f,a,b;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
 for(;;) {
  BOOL got=PeekMessageW(&message,NULL,0,0,PM_REMOVE);
  QueryPerformanceCounter(&b);
  double ms=1000.0*(double)(b.QuadPart-a.QuadPart)/(double)f.QuadPart;
  if(ms>30.0) printf("PUMP PeekMessage took %.0f ms\n",ms);
  if(!got) break;
  if(++count>4096) boundary("host-message-budget",&state,state.pc);
  TranslateMessage(&message);DispatchMessageW(&message);
  QueryPerformanceCounter(&a);
  ms=1000.0*(double)(a.QuadPart-b.QuadPart)/(double)f.QuadPart;
  if(ms>30.0) printf("PUMP message 0x%x took %.0f ms\n",message.message,ms);
 }
 /* the in-game menu: opened, it takes the mouse and keys from the game; what it asked for */
 static int menu_was;
 int open=menu_open();
 if(open!=menu_was) {
  menu_was=open;
  if(open) {
   if(GetCapture()==app_window) ReleaseCapture();
   mouse_clear();mouse_keys_clear();mouse_set_captured(app_window,0);mouse_alt_free=0;
   memset(hid_keys,0,sizeof(hid_keys));memset(keyboard_keys,0,sizeof(keyboard_keys));
  }
 }
 if(!app_cursor_idle && !mouse_captured && !open && app_input_focus && GetTickCount64()-app_mouse_moved>3000) {
  app_cursor_idle=1;SetCursor(NULL);
 }
 if(open && app_cursor_idle) {app_cursor_idle=0;SetCursor(LoadCursorW(NULL,MAKEINTRESOURCEW(32512)));}
 LONG fullscreen=InterlockedExchange(&app_menu_fullscreen,-1);
 if(fullscreen>=0) app_set_fullscreen(app_window,(int)fullscreen);
 app_window_size_live();
 if(InterlockedExchange(&app_menu_quit,0)) app_window_proc(app_window,WM_CLOSE,0,0);
 QueryPerformanceCounter(&a);
 hid_poll(app_input_focus && app_focus==1 && !app_exit_requested);
 QueryPerformanceCounter(&b);
 double ms=1000.0*(double)(b.QuadPart-a.QuadPart)/(double)f.QuadPart;
 if(ms>30.0) printf("PUMP input read took %.0f ms\n",ms);
}
static void app_checkpoint(void) {
 clock_refresh();
 app_pump();
 if(!app_window || !app_focus_policy || app_exit_requested) return;
 /* AC_IGNORE_FOCUS=1: keep running while the window is unfocused or in the background (unattended runs). */
 static int ignore_focus;
 SETTING_LIVE(ignore_focus,"AC_IGNORE_FOCUS",0);
 if(ignore_focus) return;
 uint64_t start=0;
 while((app_focus==3 && app_suspend_background) || (app_focus==2 && app_suspend_out_of_focus)) {
  if(!start) start=rt_ticks();
  unsigned held=klock_release_all();  /* (parallel mode: other threads' services must not wait on this) */
  DWORD waited=MsgWaitForMultipleObjectsEx(0,NULL,16,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
  klock_reacquire(held);
  if(waited==WAIT_FAILED)
   boundary("host-suspension-wait",&state,state.pc);
  app_pump();
  if(app_exit_requested) break;
 }
 if(start) {
  app_suspended_ticks+=rt_ticks()-start;
  if(app_suspend_event && !SetEvent(app_suspend_event)) boundary("suspended-time-event",&state,state.pc);
  if(app_resume_notifications && !app_exit_requested) app_enqueue(16);
 }
}
static void app_open(void) {
 if(app_window) return;
 if(!app_message_event) {
  app_message_event=CreateEventW(NULL,TRUE,FALSE,NULL);
  if(!app_message_event) boundary("host-applet-event-create",&state,state.pc);
 }
 WNDCLASSW window_class={0};
 window_class.lpfnWndProc=app_window_proc;
 window_class.hInstance=GetModuleHandleW(NULL);
 window_class.lpszClassName=L"AstralChainNativeStartup";
 window_class.hCursor=LoadCursorW(NULL,MAKEINTRESOURCEW(32512));
 window_class.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
 if(!RegisterClassW(&window_class)) boundary("host-window-class",&state,state.pc);
 app_window=CreateWindowExW(0,window_class.lpszClassName,L"Astral Chain native startup - graphics not implemented",
  WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,960,540,NULL,NULL,window_class.hInstance,NULL);
 if(!app_window) boundary("host-window-create",&state,state.pc);
 app_window_thread=GetCurrentThreadId();
 ShowWindow(app_window,SW_SHOWNOACTIVATE);UpdateWindow(app_window);app_pump();
 app_set_focus(IsIconic(app_window) ? 3 : 1);
 /* Publish initial application state even if no size transition occurred. */
 if(!app_message_count) app_enqueue(15);
}
static DWORD app_wait(DWORD count,const HANDLE *handles,DWORD timeout) {
 if(!app_window) return WaitForMultipleObjects(count,handles,FALSE,timeout);
 ULONGLONG start=GetTickCount64();
 for(;;) {
  app_pump();
  DWORD result=WaitForMultipleObjects(count,handles,FALSE,0);
  if(result!=WAIT_TIMEOUT) return result;
  ULONGLONG elapsed=GetTickCount64()-start;
  if(timeout!=INFINITE && elapsed>=timeout) return WAIT_TIMEOUT;
  DWORD slice=timeout==INFINITE ? 16 : (DWORD)((timeout-elapsed)<16 ? timeout-elapsed : 16);
  if(MsgWaitForMultipleObjectsEx(0,NULL,slice,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==WAIT_FAILED) return WAIT_FAILED;
 }
}
