// Window layer (WM_ namespace). One Win32 window, a DPI-aware creation
// path, and a batched event pump (wm_get_events) rather than a per-message
// callback - callers drain a list once per frame. Modeled on raddebugger's
// window_manager layer, trimmed to a single window and the handful of
// events this app needs (close, resize).

#ifndef WM_CORE_H
#define WM_CORE_H

typedef struct WM_Window WM_Window;
struct WM_Window
{
  U64 u64[1]; // opaque - wraps an HWND
};

typedef U32 WM_EventKind;
enum
{
  WM_EventKind_Close,
  WM_EventKind_Resize,
};

typedef struct WM_Event WM_Event;
struct WM_Event
{
  WM_Event *next;
  WM_EventKind kind;
  U32 width;  // WM_EventKind_Resize: new client-area size, in pixels
  U32 height;
};

typedef struct WM_EventList WM_EventList;
struct WM_EventList
{
  WM_Event *first;
  WM_Event *last;
  U64 count;
};

// Sets per-monitor DPI awareness. Call once, before wm_window_open.
internal void wm_init(void);

internal WM_Window wm_window_open(String8 title, U32 width, U32 height);
internal void      wm_window_close(WM_Window window);
internal void      wm_client_size(WM_Window window, U32 *out_width, U32 *out_height);
internal void     *wm_native_handle(WM_Window window); // HWND, for the renderer to equip a swapchain against

// Pumps the Win32 message queue (non-blocking) and returns whatever
// close/resize events arrived since the last call, allocated into `arena`.
internal WM_EventList wm_get_events(Arena *arena);

// Polled (not event-queued) cursor position (client-area pixels) and left
// button down-state, fed by the same WndProc as the events above. Call
// once per frame - fits the immediate-mode convention of asking "what is
// the mouse doing right now" rather than routing clicks through events.
internal void wm_mouse_state(WM_Window window, F32 *out_x, F32 *out_y, B32 *out_left_down);

// Accumulated wheel notches (positive = away from user / "scroll up")
// since the last call - reads-and-resets, same polled-once-per-frame
// convention as wm_mouse_state.
internal F32 wm_mouse_wheel_delta(void);

#endif // WM_CORE_H
