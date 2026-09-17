// Single-window Win32 implementation. WndProc runs on the same thread that
// calls wm_get_events (the main/render thread), so the raw pending-event
// buffer below needs no locking.

global B32     wm_g_class_registered = 0;
global HWND    wm_g_hwnd = 0;
global WM_Event wm_g_raw_events[64];
global U64      wm_g_raw_event_count;

global F32 wm_g_mouse_x = 0.0f;
global F32 wm_g_mouse_y = 0.0f;
global B32 wm_g_mouse_left_down = 0;
global F32 wm_g_wheel_delta = 0.0f;

internal LRESULT CALLBACK
wm_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  LRESULT result = 0;
  switch(msg)
  {
    default:
    {
      result = DefWindowProcW(hwnd, msg, wparam, lparam);
    }break;

    case WM_CLOSE:
    {
      if(wm_g_raw_event_count < ArrayCount(wm_g_raw_events))
      {
        wm_g_raw_events[wm_g_raw_event_count].kind = WM_EventKind_Close;
        wm_g_raw_event_count += 1;
      }
      DestroyWindow(hwnd);
    }break;

    case WM_DESTROY:
    {
      PostQuitMessage(0);
    }break;

    case WM_SIZE:
    {
      if(wm_g_raw_event_count < ArrayCount(wm_g_raw_events))
      {
        wm_g_raw_events[wm_g_raw_event_count].kind = WM_EventKind_Resize;
        wm_g_raw_events[wm_g_raw_event_count].width = LOWORD(lparam);
        wm_g_raw_events[wm_g_raw_event_count].height = HIWORD(lparam);
        wm_g_raw_event_count += 1;
      }
    }break;

    case WM_MOUSEMOVE:
    {
      // cast through S16 first: client coords can go negative while
      // dragging with the mouse captured outside the window
      wm_g_mouse_x = (F32)(S16)LOWORD(lparam);
      wm_g_mouse_y = (F32)(S16)HIWORD(lparam);
    }break;

    case WM_LBUTTONDOWN:
    {
      wm_g_mouse_left_down = 1;
      SetCapture(hwnd); // keep tracking button-up/move even if dragged outside the window
    }break;

    case WM_LBUTTONUP:
    {
      wm_g_mouse_left_down = 0;
      ReleaseCapture();
    }break;

    case WM_MOUSEWHEEL:
    {
      S16 delta_raw = (S16)HIWORD(wparam);
      wm_g_wheel_delta += (F32)delta_raw / (F32)WHEEL_DELTA;
    }break;

    case WM_CHAR:
    {
      // Control characters (backspace, tab, enter, escape, ...) also arrive
      // here as well as WM_KEYDOWN - only take the printable ones from
      // WM_CHAR and handle editing keys exclusively via WM_KEYDOWN below, so
      // a single keypress doesn't get handled twice.
      if(wparam >= 0x20 && wm_g_raw_event_count < ArrayCount(wm_g_raw_events))
      {
        wm_g_raw_events[wm_g_raw_event_count].kind = WM_EventKind_Char;
        wm_g_raw_events[wm_g_raw_event_count].code = (U32)wparam;
        wm_g_raw_event_count += 1;
      }
    }break;

    case WM_KEYDOWN:
    {
      if(wm_g_raw_event_count < ArrayCount(wm_g_raw_events))
      {
        wm_g_raw_events[wm_g_raw_event_count].kind = WM_EventKind_KeyDown;
        wm_g_raw_events[wm_g_raw_event_count].code = (U32)wparam;
        wm_g_raw_event_count += 1;
      }
    }break;
  }
  return result;
}

internal void
wm_init(void)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

internal WM_Window
wm_window_open(String8 title, U32 width, U32 height)
{
  HINSTANCE instance = GetModuleHandleW(0);

  if(!wm_g_class_registered)
  {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wm_wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"lwftpclient_wm_window";
    RegisterClassExW(&wc);
    wm_g_class_registered = 1;
  }

  Temp scratch = scratch_begin(0, 0);
  int wide_count = MultiByteToWideChar(CP_UTF8, 0, (char *)title.str, (int)title.size, 0, 0);
  WCHAR *wide_title = push_array_no_zero(scratch.arena, WCHAR, wide_count + 1);
  MultiByteToWideChar(CP_UTF8, 0, (char *)title.str, (int)title.size, wide_title, wide_count);
  wide_title[wide_count] = 0;

  RECT rect = {0, 0, (LONG)width, (LONG)height};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

  HWND hwnd = CreateWindowExW(0, L"lwftpclient_wm_window", wide_title,
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top,
                               0, 0, instance, 0);
  scratch_end(scratch);

  ShowWindow(hwnd, SW_SHOW);
  wm_g_hwnd = hwnd;

  WM_Window result = {(U64)hwnd};
  return result;
}

internal void
wm_window_close(WM_Window window)
{
  DestroyWindow((HWND)PtrFromInt(window.u64[0]));
}

internal void
wm_client_size(WM_Window window, U32 *out_width, U32 *out_height)
{
  RECT rect = {0};
  GetClientRect((HWND)PtrFromInt(window.u64[0]), &rect);
  *out_width = (U32)(rect.right - rect.left);
  *out_height = (U32)(rect.bottom - rect.top);
}

internal void *
wm_native_handle(WM_Window window)
{
  return PtrFromInt(window.u64[0]);
}

internal void
wm_mouse_state(WM_Window window, F32 *out_x, F32 *out_y, B32 *out_left_down)
{
  (void)window; // single-window app - state is process-global for now
  *out_x = wm_g_mouse_x;
  *out_y = wm_g_mouse_y;
  *out_left_down = wm_g_mouse_left_down;
}

internal F32
wm_mouse_wheel_delta(void)
{
  F32 result = wm_g_wheel_delta;
  wm_g_wheel_delta = 0.0f;
  return result;
}

internal WM_EventList
wm_get_events(Arena *arena)
{
  MSG msg;
  while(PeekMessageW(&msg, 0, 0, 0, PM_REMOVE))
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  WM_EventList list = {0};
  for(U64 i = 0; i < wm_g_raw_event_count; i += 1)
  {
    WM_Event *node = push_array_no_zero(arena, WM_Event, 1);
    *node = wm_g_raw_events[i];
    node->next = 0;
    SLLQueuePush(list.first, list.last, node);
    list.count += 1;
  }
  wm_g_raw_event_count = 0;

  return list;
}
