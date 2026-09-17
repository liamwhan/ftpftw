// UI structure: toolbar (now with a Connections button), local/remote
// directory-tree panes, tabbed bottom pane (Log/Queue/Failed/Completed).
// sftp_client.h/.c are unchanged - the control thread's GuardedRing
// protocol doesn't know or care that its Entry messages become tree rows,
// or that connections can now come from a saved profile instead of env
// vars.

#include "base/base_inc.h"
#include "base/base_inc.c"

#include "wm/wm_core.h"
#include "wm/win32/wm_core_win32.c"

#include "render/r_core.h"
#include "render/d3d11/r_d3d11.c"

// fp_dwrite is implemented in fp_dwrite.cpp, compiled and linked
// separately (see build.bat) - <dwrite.h> isn't C-includable, see
// fp_dwrite.h for why. Only its plain-C-callable header comes in here.
#include "font/fp_dwrite.h"
#include "font/fnt_cache.h"
#include "font/fnt_cache.c"

#include "draw/dr_core.h"
#include "draw/dr_core.c"

#include "fs/fs_local.h"
#include "fs/fs_local.c"

#include "ui/ui_icons.h"
#include "ui/ui_icons.c"
#include "ui/ui_core.h"
#include "ui/ui_core.c"

#include "store/conn_store.h"
#include "store/conn_store.c"

#include "ui/ui_conn_modal.h"
#include "ui/ui_conn_modal.c"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "sftp/sftp_client.h"
#include "sftp/sftp_client.c"

#define TOOLBAR_H     40.0f
#define BOTTOM_PANE_H 200.0f
#define TAB_STRIP_H   28.0f
#define ROW_H         22.0f
#define MAX_REMOTE_ENTRIES 1024

internal String8
env_str8(Arena *arena, char *name)
{
  char *value = getenv(name);
  return value ? str8_cstring_copy(arena, value) : (String8){0};
}

// Log tab text: Log/Error messages only - Entry messages become remote
// tree rows (see RemoteConn below) instead of log-style text lines.
internal String8
line_from_ctrl_msg(Arena *arena, SFTP_CtrlMsg *msg, U64 log_start_us)
{
  String8 result = {0};
  F64 elapsed_s = (F64)(msg->timestamp_us - log_start_us) / 1000000.0;
  switch(msg->kind)
  {
    default: break;
    case SFTP_CtrlMsgKind_Log:
    {
      result = str8f(arena, "[%7.3f] %.*s", elapsed_s, (int)msg->text_size, (char *)msg->text);
    }break;
    case SFTP_CtrlMsgKind_Error:
    {
      result = str8f(arena, "[%7.3f] error: %.*s", elapsed_s, (int)msg->text_size, (char *)msg->text);
    }break;
  }
  return result;
}

// Allocates a fresh session arena and launches a new control-connection
// thread against it - used both by connect_to (below) and the reconnect
// timer. The GuardedRings and SFTP_ConnectParams must stay valid for the
// whole life of the thread (it reads params->out_ring/in_ring throughout,
// not just at startup), so they're pushed from session_arena rather than
// being stack locals here - session_arena itself is only released after
// the thread has fully exited (ThreadExit/Disconnected).
internal Thread
start_sftp_session(Arena *session_arena, String8 host, U16 port, String8 user, String8 pass,
                    String8 remote_dir, GuardedRing **out_ring, GuardedRing **out_in_ring)
{
  GuardedRing *ring = push_array(session_arena, GuardedRing, 1);
  *ring = guarded_ring_alloc(session_arena, KB(64));
  GuardedRing *in_ring = push_array(session_arena, GuardedRing, 1);
  *in_ring = guarded_ring_alloc(session_arena, KB(4));

  SFTP_ConnectParams *params = push_array(session_arena, SFTP_ConnectParams, 1);
  params->host = host;
  params->port = port;
  params->username = user;
  params->password = pass;
  params->remote_dir = remote_dir;
  params->out_ring = ring;
  params->in_ring = in_ring;

  *out_ring = ring;
  *out_in_ring = in_ring;
  return thread_launch(sftp_control_thread_entry, params);
}

// Everything about "the current remote connection and what the remote
// pane is showing" bundled together - these were 15 separate locals in
// main() before the Connections modal made "connect to a different target
// at runtime" a real operation, not just a startup-time constant; grouping
// them is what actually keeps connect_to/teardown_live_session's
// signatures flat rather than each taking a dozen individual pointers.
typedef struct RemoteConn RemoteConn;
struct RemoteConn
{
  // session/thread lifetime
  Arena *session_arena; // torn down and recreated per connection attempt
  GuardedRing *ring;
  GuardedRing *in_ring;
  Thread control;
  B32 control_joined;
  B32 connecting;         // true once any connection attempt has ever been made - never goes back to false
  B32 awaiting_reconnect;
  U64 reconnect_at_us;
  U64 log_start_us;

  // credentials - copied into creds_arena, which outlives session_arena
  // (a reconnect to the same target reuses these; connect_to replaces them)
  Arena *creds_arena;
  String8 host, user, pass;
  U16 port;

  // remote pane display state - reset on every connect_to, cleared and
  // repopulated on every navigation/reconnect
  Arena *remote_arena;
  String8 remote_path;
  FS_Entry entries[MAX_REMOTE_ENTRIES];
  U64 entry_count;
  B32 sorted;
  UI_RowListState row_state;
};

// Signals the live control thread to quit and blocks until it's fully
// exited, then releases its rings and session arena. Used both by
// connect_to (switching to a different target while connected) and final
// shutdown. Blocking here means a brief UI freeze while the old
// connection tears down (session disconnect, socket close) - acceptable
// for a first pass; a fully non-blocking teardown would need its own
// polled state machine, not worth it yet for what's normally sub-second.
internal void
teardown_live_session(RemoteConn *rc)
{
  SFTP_Req quit_req = {0};
  quit_req.kind = SFTP_ReqKind_Quit;
  guarded_ring_write_struct_or_wait(rc->in_ring, &quit_req, max_U64);

  for(;;)
  {
    SFTP_CtrlMsg msg = {0};
    guarded_ring_read_struct_or_wait(rc->ring, &msg, max_U64);
    if(msg.kind == SFTP_CtrlMsgKind_ThreadExit || msg.kind == SFTP_CtrlMsgKind_Disconnected)
    {
      break;
    }
  }
  thread_join(rc->control, max_U64);
  guarded_ring_release(rc->ring);
  guarded_ring_release(rc->in_ring);
  arena_release(rc->session_arena);
}

// Connects (or reconnects to a *different* target) - tears down whatever's
// currently live first, always starts fresh at the root directory (unlike
// the automatic reconnect-on-disconnect timer, which resumes at whatever
// remote_path was last showing for the *same* target). Called both at
// startup (env-var fallback) and from the Connections modal's Connect
// button.
internal void
connect_to(RemoteConn *rc, String8 host, U16 port, String8 user, String8 pass)
{
  if(!rc->control_joined)
  {
    teardown_live_session(rc);
  }
  rc->awaiting_reconnect = 0;

  arena_clear(rc->creds_arena);
  rc->host = str8_copy(rc->creds_arena, host);
  rc->user = str8_copy(rc->creds_arena, user);
  rc->pass = str8_copy(rc->creds_arena, pass);
  rc->port = port;

  arena_clear(rc->remote_arena);
  rc->remote_path = str8_copy(rc->remote_arena, str8_lit("."));
  rc->entry_count = 0;
  rc->sorted = 0;
  rc->row_state.last_click_index = -1;
  rc->row_state.scroll_y = 0.0f;

  rc->log_start_us = now_time_us();
  rc->session_arena = arena_alloc();
  rc->control = start_sftp_session(rc->session_arena, rc->host, rc->port, rc->user, rc->pass, rc->remote_path,
                                    &rc->ring, &rc->in_ring);
  rc->control_joined = 0;
  rc->connecting = 1;
}

int
main(void)
{
  os_init();

  TCTX *tctx = tctx_alloc();
  tctx_select(tctx);
  set_thread_name(str8_lit("main"));

  wm_init();
  WM_Window window = wm_window_open(str8_lit("lwftpclient"), 1100, 720);

  r_init();
  r_window_equip(window);

  fp_init();
  fnt_init();
  ui_icons_init();
  FP_Font font = fp_font_open("Segoe UI");

  Arena *arena = arena_alloc();        // app-lifetime: log lines, saved connection profiles
  Arena *frame_arena = arena_alloc();  // cleared every frame
  Arena *local_arena = arena_alloc();  // local path/listing - cleared on every navigation

  // --- local pane state: starts at %USERPROFILE%, navigable -----------------
  String8 local_path = str8_copy(local_arena, env_str8(frame_arena, "USERPROFILE"));
  FS_EntryArray local_entries = fs_list_dir(local_arena, local_path);
  UI_RowListState local_row_state = {0};
  local_row_state.last_click_index = -1;

  // --- Connections modal ------------------------------------------------------
  UI_ConnModal conn_modal = {0};
  ui_conn_modal_init(&conn_modal, arena);

  // --- remote connection + pane state ------------------------------------------
  // libssh2_init/exit bracket the whole process lifetime exactly once,
  // independent of whether/when/how many times we actually connect
  // (env vars at startup, never, or only later via the modal).
  libssh2_init(0);

  RemoteConn rc = {0};
  rc.control_joined = 1;
  rc.creds_arena = arena_alloc();
  rc.remote_arena = arena_alloc();
  rc.row_state.last_click_index = -1;

  String8List lines = {0};

  {
    String8 env_host = env_str8(frame_arena, "FTP_HOST");
    String8 env_user = env_str8(frame_arena, "FTP_USER");
    String8 env_pass = env_str8(frame_arena, "FTP_PASS");
    String8 env_port_str = env_str8(frame_arena, "FTP_PORT");
    if(env_host.size != 0 && env_user.size != 0 && env_pass.size != 0)
    {
      U16 env_port = env_port_str.size ? (U16)atoi((char *)env_port_str.str) : 22;
      if(env_port == 0)
      {
        env_port = 22;
      }
      connect_to(&rc, env_host, env_port, env_user, env_pass);
    }
    else
    {
      str8_list_push(arena, &lines,
                      str8_lit("not connected - set FTP_HOST/FTP_USER/FTP_PASS(/FTP_PORT) for a dev auto-connect, "
                               "or use the Connections button in the toolbar"));
    }
  }

  S32 bottom_tab = 0;
  String8 tab_labels[4] = { str8_lit("Log"), str8_lit("Queue"), str8_lit("Failed"), str8_lit("Completed") };

  // Log tab: scroll_from_top in [0, max_scroll], auto-follows the latest
  // line (scroll_from_top snapped to max_scroll every frame) until the
  // user scrolls the wheel over it, same as a terminal/log viewer.
  F32 log_scroll_from_top = 0.0f;
  B32 log_auto_follow = 1;

  B32 should_quit = 0;
  while(!should_quit)
  {
    arena_clear(frame_arena);
    WM_EventList events = wm_get_events(frame_arena);
    for(WM_Event *ev = events.first; ev != 0; ev = ev->next)
    {
      switch(ev->kind)
      {
        default: break;
        case WM_EventKind_Close:  { should_quit = 1; }break;
        case WM_EventKind_Resize: { r_window_resize(ev->width, ev->height); }break;
      }
    }
    if(should_quit)
    {
      break;
    }

    ui_begin_frame(window);

    // Drain whatever the control thread has produced since last frame -
    // non-blocking (see GuardedRing docs): Entry messages become remote
    // tree rows, Log/Error become Log-tab text lines.
    if(rc.connecting && !rc.control_joined)
    {
      for(;;)
      {
        SFTP_CtrlMsg msg = {0};
        if(!guarded_ring_read_struct_or_wait(rc.ring, &msg, now_time_us()))
        {
          break;
        }
        if(msg.kind == SFTP_CtrlMsgKind_Entry)
        {
          if(rc.entry_count < ArrayCount(rc.entries))
          {
            FS_Entry *e = &rc.entries[rc.entry_count];
            e->name = str8_copy(rc.remote_arena, str8(msg.name, msg.name_size));
            e->is_dir = msg.is_dir;
            e->size = msg.size_bytes;
            rc.entry_count += 1;
          }
        }
        else
        {
          String8 line = line_from_ctrl_msg(arena, &msg, rc.log_start_us);
          if(line.size != 0)
          {
            str8_list_push(arena, &lines, line);
          }
        }
        if(msg.kind == SFTP_CtrlMsgKind_Done)
        {
          if(!rc.sorted)
          {
            qsort(rc.entries, rc.entry_count, sizeof(FS_Entry), fs_entry_compare);
            rc.sorted = 1;
          }
        }
        if(msg.kind == SFTP_CtrlMsgKind_ThreadExit)
        {
          // Deliberate Quit (window close, or connect_to switching
          // targets - both drain to ThreadExit/Disconnected themselves
          // via teardown_live_session, so this is the "died on its own"
          // case) or bad credentials - either way, don't reconnect.
          thread_join(rc.control, max_U64);
          rc.control_joined = 1;
          break;
        }
        if(msg.kind == SFTP_CtrlMsgKind_Disconnected)
        {
          thread_join(rc.control, max_U64);
          rc.control_joined = 1;
          guarded_ring_release(rc.ring);
          guarded_ring_release(rc.in_ring);
          arena_release(rc.session_arena);
          rc.session_arena = 0;
          rc.ring = 0;
          rc.in_ring = 0;
          arena_clear(rc.remote_arena);
          rc.entry_count = 0;
          rc.sorted = 0;
          rc.row_state.last_click_index = -1;
          rc.awaiting_reconnect = 1;
          rc.reconnect_at_us = now_time_us() + 2 * 1000000;
          str8_list_push(arena, &lines, str8_lit("reconnecting in 2s..."));
          break;
        }
      }
    }

    // A reconnect never fires the instant it's scheduled - a short delay
    // avoids hammering an unreachable server in a tight fail loop. Resumes
    // at the same remote_path (unlike connect_to, which always resets to
    // root) since this is the *same* target coming back, not a switch.
    if(rc.connecting && rc.awaiting_reconnect && now_time_us() >= rc.reconnect_at_us)
    {
      rc.log_start_us = now_time_us();
      rc.session_arena = arena_alloc();
      rc.control = start_sftp_session(rc.session_arena, rc.host, rc.port, rc.user, rc.pass, rc.remote_path,
                                       &rc.ring, &rc.in_ring);
      rc.control_joined = 0;
      rc.awaiting_reconnect = 0;
    }

    // --- layout ---------------------------------------------------------------
    U32 win_w_u, win_h_u;
    wm_client_size(window, &win_w_u, &win_h_u);
    F32 win_w = (F32)win_w_u, win_h = (F32)win_h_u;

    F32 mid_y0 = TOOLBAR_H;
    F32 mid_y1 = Max(mid_y0, win_h - BOTTOM_PANE_H);
    F32 col_split_x = win_w * 0.5f;
    F32 bottom_y0 = mid_y1;

    dr_begin_frame(0.08f, 0.08f, 0.10f, 1.0f);

    // toolbar
    dr_rect(0, 0, win_w, TOOLBAR_H, 0.16f, 0.16f, 0.18f, 1.0f);
    dr_rect(0, TOOLBAR_H, win_w, TOOLBAR_H + 1.0f, 0.05f, 0.05f, 0.06f, 1.0f);
    {
      F32 conn_u0, conn_v0, conn_u1, conn_v1;
      ui_icon_conn_uv(&conn_u0, &conn_v0, &conn_u1, &conn_v1);
      // Input gating: while the modal is open, the toolbar button itself
      // stays live (so Close/re-toggle still works via it if wanted) but
      // clicking it while already open would just re-open onto itself -
      // harmless - so no extra guard needed here specifically.
      if(ui_icon_button(6.0f, 6.0f, TOOLBAR_H - 12.0f, conn_u0, conn_v0, conn_u1, conn_v1))
      {
        conn_modal.open = !conn_modal.open;
      }
    }

    // divider between the two panes
    dr_rect(col_split_x - 1.0f, mid_y0, col_split_x + 1.0f, mid_y1, 0.05f, 0.05f, 0.06f, 1.0f);
    // divider above the bottom pane
    dr_rect(0, bottom_y0, win_w, bottom_y0 + 1.0f, 0.05f, 0.05f, 0.06f, 1.0f);
    // opaque bottom-pane backdrop - belt-and-suspenders alongside the tree
    // panes now being clipped to their own rects (see ui_row_list): nothing
    // above should ever be able to show through here regardless.
    dr_rect(0, bottom_y0 + 1.0f, win_w, win_h, 0.08f, 0.08f, 0.10f, 1.0f);

    // Input gating while the modal is open: skip pane/tab interaction
    // entirely (still draw them - just dimmed by the modal's own overlay -
    // don't act on clicks landing behind it).
    B32 input_gated = conn_modal.open;

    // --- local pane -------------------------------------------------------------
    dr_text(&font, 13.0f, 10.0f, ui_text_baseline_y(mid_y0, 26.0f, 13.0f), 0.55f, 0.55f, 0.55f, 1.0f, str8_lit("Local"));
    F32 local_list_y = mid_y0 + 26.0f;
    F32 local_list_h = Max(0.0f, mid_y1 - local_list_y);
    B32 local_dbl = 0;
    S32 local_clicked = ui_row_list(frame_arena, &font, 6.0f, local_list_y, col_split_x - 12.0f, local_list_h, ROW_H,
                                      local_entries.v, local_entries.count, &local_row_state, &local_dbl);
    if(!input_gated && local_dbl && local_clicked >= 0 && (U64)local_clicked < local_entries.count)
    {
      FS_Entry *e = &local_entries.v[local_clicked];
      if(e->is_dir)
      {
        String8 new_path = str8_match(e->name, str8_lit(".."))
                          ? fs_path_parent(frame_arena, local_path)
                          : fs_path_join(frame_arena, local_path, e->name);
        arena_clear(local_arena);
        local_path = str8_copy(local_arena, new_path);
        local_entries = fs_list_dir(local_arena, local_path);
        local_row_state.last_click_index = -1;
      }
    }

    // --- remote pane --------------------------------------------------------------
    if(rc.connecting)
    {
      dr_text(&font, 13.0f, col_split_x + 10.0f, ui_text_baseline_y(mid_y0, 26.0f, 13.0f), 0.55f, 0.55f, 0.55f, 1.0f, rc.host);
    }
    F32 remote_list_y = mid_y0 + 26.0f;
    F32 remote_list_h = Max(0.0f, mid_y1 - remote_list_y);
    B32 remote_dbl = 0;
    S32 remote_clicked = ui_row_list(frame_arena, &font, col_split_x + 6.0f, remote_list_y, col_split_x - 12.0f, remote_list_h, ROW_H,
                                       rc.entries, rc.entry_count, &rc.row_state, &remote_dbl);
    if(!input_gated && rc.connecting && !rc.control_joined && remote_dbl && remote_clicked >= 0 && (U64)remote_clicked < rc.entry_count)
    {
      FS_Entry *e = &rc.entries[remote_clicked];
      if(e->is_dir && !str8_match(e->name, str8_lit(".")))
      {
        String8 new_path = str8_match(e->name, str8_lit(".."))
                          ? sftp_path_parent(frame_arena, rc.remote_path)
                          : sftp_path_join(frame_arena, rc.remote_path, e->name);
        arena_clear(rc.remote_arena);
        rc.remote_path = str8_copy(rc.remote_arena, new_path);
        rc.entry_count = 0;
        rc.sorted = 0;
        rc.row_state.last_click_index = -1;

        SFTP_Req req = {0};
        req.kind = SFTP_ReqKind_ListDir;
        req.path_size = Min(rc.remote_path.size, sizeof(req.path));
        MemoryCopy(req.path, rc.remote_path.str, req.path_size);
        guarded_ring_write_struct_or_wait(rc.in_ring, &req, max_U64);
      }
    }

    // --- bottom pane: tab strip + content ------------------------------------------
    S32 tab_clicked = ui_tab_strip(&font, 0, bottom_y0 + 1.0f, win_w / 4.0f, TAB_STRIP_H, tab_labels, 4, bottom_tab);
    if(!input_gated && tab_clicked >= 0)
    {
      bottom_tab = tab_clicked;
    }

    F32 content_y0 = bottom_y0 + 1.0f + TAB_STRIP_H;
    F32 content_h = Max(0.0f, win_h - content_y0);
    dr_set_clip(0, content_y0, win_w, win_h);
    switch(bottom_tab)
    {
      default: break;
      case 0: // Log - scrollable, auto-follows the latest line until the user scrolls
      {
        F32 line_h = 18.0f;
        F32 log_content_h = (F32)lines.node_count * line_h;
        F32 log_max_scroll = Max(0.0f, log_content_h - content_h);

        B32 mouse_in_log = !input_gated && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, content_y0, win_w, win_h);
        if(mouse_in_log)
        {
          F32 wheel = wm_mouse_wheel_delta();
          if(wheel != 0.0f)
          {
            log_auto_follow = 0;
            log_scroll_from_top -= wheel * line_h * 3.0f;
          }
        }
        if(log_auto_follow)
        {
          log_scroll_from_top = log_max_scroll;
        }
        log_scroll_from_top = Clamp(0.0f, log_scroll_from_top, log_max_scroll);
        if(log_scroll_from_top >= log_max_scroll)
        {
          log_auto_follow = 1; // scrolled back to the bottom - resume following
        }

        U64 idx = 0;
        F32 y_top = content_y0 + 4.0f - log_scroll_from_top;
        for(String8Node *n = lines.first; n != 0; n = n->next)
        {
          F32 line_y = y_top + (F32)idx * line_h;
          if(line_y + line_h >= content_y0 && line_y <= win_h)
          {
            dr_text(&font, 14.0f, 10.0f, ui_text_baseline_y(line_y, line_h, 14.0f), 0.8f, 0.8f, 0.8f, 1.0f, n->string);
          }
          idx += 1;
        }
      }break;
      case 1: { dr_text(&font, 14.0f, 10.0f, ui_text_baseline_y(content_y0, 26.0f, 14.0f), 0.5f, 0.5f, 0.5f, 1.0f, str8_lit("No transfers in progress.")); }break;
      case 2: { dr_text(&font, 14.0f, 10.0f, ui_text_baseline_y(content_y0, 26.0f, 14.0f), 0.5f, 0.5f, 0.5f, 1.0f, str8_lit("No failed transfers.")); }break;
      case 3: { dr_text(&font, 14.0f, 10.0f, ui_text_baseline_y(content_y0, 26.0f, 14.0f), 0.5f, 0.5f, 0.5f, 1.0f, str8_lit("No completed transfers.")); }break;
    }
    dr_clear_clip();

    // --- Connections modal (drawn last so it's on top of everything else) -----------
    {
      String8 modal_host = {0}, modal_user = {0}, modal_pass = {0};
      U16 modal_port = 22;
      if(ui_conn_modal_update(&conn_modal, arena, rc.creds_arena, &events, &font, win_w, win_h,
                               &modal_host, &modal_port, &modal_user, &modal_pass))
      {
        connect_to(&rc, modal_host, modal_port, modal_user, modal_pass);
      }
    }

    dr_end_frame();
    ui_end_frame();
  }

  if(rc.connecting && !rc.control_joined)
  {
    teardown_live_session(&rc);
  }
  // else: either never connected, or currently sitting in the
  // awaiting_reconnect gap between sessions - no live thread either way,
  // nothing to signal or join.
  libssh2_exit();

  return 0;
}
