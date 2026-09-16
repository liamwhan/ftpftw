// UI structure: toolbar, local/remote directory-tree panes, tabbed bottom
// pane (Log/Queue/Failed/Completed). sftp_client.h/.c are unchanged - the
// control thread's GuardedRing protocol doesn't know or care that its
// Entry messages now become tree rows instead of log-style text lines.

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

// Log tab text: Log/Error messages only now - Entry messages become tree
// rows (see main loop below) instead of log-style text lines.
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

  Arena *arena = arena_alloc();        // app-lifetime: connect params, log lines
  Arena *frame_arena = arena_alloc();  // cleared every frame
  Arena *local_arena = arena_alloc();  // local path/listing - cleared on every navigation
  Arena *remote_arena = arena_alloc(); // remote path/listing - cleared on every navigation

  // --- local pane state: starts at %USERPROFILE%, navigable -----------------
  String8 local_path = str8_copy(local_arena, env_str8(frame_arena, "USERPROFILE"));
  FS_EntryArray local_entries = fs_list_dir(local_arena, local_path);
  UI_RowListState local_row_state = {0};
  local_row_state.last_click_index = -1;

  // --- remote pane + log state ------------------------------------------------
  String8 host = env_str8(arena, "FTP_HOST");
  String8 user = env_str8(arena, "FTP_USER");
  String8 pass = env_str8(arena, "FTP_PASS");
  String8 port_str = env_str8(arena, "FTP_PORT");
  U16 port = port_str.size ? (U16)atoi((char *)port_str.str) : 22;
  if(port == 0)
  {
    port = 22;
  }

  String8List lines = {0};
  B32 connecting = (host.size != 0 && user.size != 0 && pass.size != 0);
  if(!connecting)
  {
    str8_list_push(arena, &lines, str8_lit("set FTP_HOST, FTP_USER, FTP_PASS (and optionally FTP_PORT), then relaunch"));
  }

  String8 remote_path = str8_copy(remote_arena, str8_lit("."));
  FS_Entry remote_entries_buf[MAX_REMOTE_ENTRIES];
  U64 remote_entry_count = 0;
  B32 remote_sorted = 0;
  UI_RowListState remote_row_state = {0};
  remote_row_state.last_click_index = -1;

  // Log tab: scroll_from_top in [0, max_scroll], auto-follows the latest
  // line (scroll_from_top snapped to max_scroll every frame) until the
  // user scrolls the wheel over it, same as a terminal/log viewer.
  F32 log_scroll_from_top = 0.0f;
  B32 log_auto_follow = 1;

  GuardedRing ring = {0};
  GuardedRing in_ring = {0};
  SFTP_ConnectParams params = {0};
  Thread control = {0};
  B32 control_joined = 1;
  U64 log_start_us = 0;
  if(connecting)
  {
    libssh2_init(0);
    ring = guarded_ring_alloc(arena, KB(64));
    in_ring = guarded_ring_alloc(arena, KB(4));
    params.host = host;
    params.port = port;
    params.username = user;
    params.password = pass;
    params.remote_dir = remote_path;
    params.out_ring = &ring;
    params.in_ring = &in_ring;
    log_start_us = now_time_us();
    control = thread_launch(sftp_control_thread_entry, &params);
    control_joined = 0;
  }

  S32 bottom_tab = 0;
  String8 tab_labels[4] = { str8_lit("Log"), str8_lit("Queue"), str8_lit("Failed"), str8_lit("Completed") };

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
    if(connecting && !control_joined)
    {
      for(;;)
      {
        SFTP_CtrlMsg msg = {0};
        if(!guarded_ring_read_struct_or_wait(&ring, &msg, now_time_us()))
        {
          break;
        }
        if(msg.kind == SFTP_CtrlMsgKind_Entry)
        {
          if(remote_entry_count < ArrayCount(remote_entries_buf))
          {
            FS_Entry *e = &remote_entries_buf[remote_entry_count];
            e->name = str8_copy(remote_arena, str8(msg.name, msg.name_size));
            e->is_dir = msg.is_dir;
            e->size = msg.size_bytes;
            remote_entry_count += 1;
          }
        }
        else
        {
          String8 line = line_from_ctrl_msg(arena, &msg, log_start_us);
          if(line.size != 0)
          {
            str8_list_push(arena, &lines, line);
          }
        }
        if(msg.kind == SFTP_CtrlMsgKind_Done)
        {
          if(!remote_sorted)
          {
            qsort(remote_entries_buf, remote_entry_count, sizeof(FS_Entry), fs_entry_compare);
            remote_sorted = 1;
          }
        }
        if(msg.kind == SFTP_CtrlMsgKind_ThreadExit)
        {
          thread_join(control, max_U64);
          control_joined = 1;
          break;
        }
      }
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

    // toolbar (empty for now - populated with icon buttons later)
    dr_rect(0, 0, win_w, TOOLBAR_H, 0.16f, 0.16f, 0.18f, 1.0f);
    dr_rect(0, TOOLBAR_H, win_w, TOOLBAR_H + 1.0f, 0.05f, 0.05f, 0.06f, 1.0f);

    // divider between the two panes
    dr_rect(col_split_x - 1.0f, mid_y0, col_split_x + 1.0f, mid_y1, 0.05f, 0.05f, 0.06f, 1.0f);
    // divider above the bottom pane
    dr_rect(0, bottom_y0, win_w, bottom_y0 + 1.0f, 0.05f, 0.05f, 0.06f, 1.0f);
    // opaque bottom-pane backdrop - belt-and-suspenders alongside the tree
    // panes now being clipped to their own rects (see ui_row_list): nothing
    // above should ever be able to show through here regardless.
    dr_rect(0, bottom_y0 + 1.0f, win_w, win_h, 0.08f, 0.08f, 0.10f, 1.0f);

    // --- local pane -------------------------------------------------------------
    dr_text(&font, 13.0f, 10.0f, ui_text_baseline_y(mid_y0, 26.0f, 13.0f), 0.55f, 0.55f, 0.55f, 1.0f, str8_lit("Local"));
    F32 local_list_y = mid_y0 + 26.0f;
    F32 local_list_h = Max(0.0f, mid_y1 - local_list_y);
    B32 local_dbl = 0;
    S32 local_clicked = ui_row_list(frame_arena, &font, 6.0f, local_list_y, col_split_x - 12.0f, local_list_h, ROW_H,
                                      local_entries.v, local_entries.count, &local_row_state, &local_dbl);
    if(local_dbl && local_clicked >= 0 && (U64)local_clicked < local_entries.count)
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
    if(connecting)
    {
      dr_text(&font, 13.0f, col_split_x + 10.0f, ui_text_baseline_y(mid_y0, 26.0f, 13.0f), 0.55f, 0.55f, 0.55f, 1.0f, host);
    }
    F32 remote_list_y = mid_y0 + 26.0f;
    F32 remote_list_h = Max(0.0f, mid_y1 - remote_list_y);
    B32 remote_dbl = 0;
    S32 remote_clicked = ui_row_list(frame_arena, &font, col_split_x + 6.0f, remote_list_y, col_split_x - 12.0f, remote_list_h, ROW_H,
                                       remote_entries_buf, remote_entry_count, &remote_row_state, &remote_dbl);
    if(connecting && !control_joined && remote_dbl && remote_clicked >= 0 && (U64)remote_clicked < remote_entry_count)
    {
      FS_Entry *e = &remote_entries_buf[remote_clicked];
      if(e->is_dir && !str8_match(e->name, str8_lit(".")))
      {
        String8 new_path = str8_match(e->name, str8_lit(".."))
                          ? sftp_path_parent(frame_arena, remote_path)
                          : sftp_path_join(frame_arena, remote_path, e->name);
        arena_clear(remote_arena);
        remote_path = str8_copy(remote_arena, new_path);
        remote_entry_count = 0;
        remote_sorted = 0;
        remote_row_state.last_click_index = -1;

        SFTP_Req req = {0};
        req.kind = SFTP_ReqKind_ListDir;
        req.path_size = Min(remote_path.size, sizeof(req.path));
        MemoryCopy(req.path, remote_path.str, req.path_size);
        guarded_ring_write_struct_or_wait(&in_ring, &req, max_U64);
      }
    }

    // --- bottom pane: tab strip + content ------------------------------------------
    S32 tab_clicked = ui_tab_strip(&font, 0, bottom_y0 + 1.0f, win_w / 4.0f, TAB_STRIP_H, tab_labels, 4, bottom_tab);
    if(tab_clicked >= 0)
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

        B32 mouse_in_log = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, content_y0, win_w, win_h);
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

    dr_end_frame();
    ui_end_frame();
  }

  if(connecting && !control_joined)
  {
    // The control thread is (most likely) blocked waiting for the next
    // SFTP_Req - without this it would never see ThreadExit and
    // thread_join below would hang forever on window close.
    SFTP_Req quit_req = {0};
    quit_req.kind = SFTP_ReqKind_Quit;
    guarded_ring_write_struct_or_wait(&in_ring, &quit_req, max_U64);

    for(;;)
    {
      SFTP_CtrlMsg msg = {0};
      guarded_ring_read_struct_or_wait(&ring, &msg, max_U64);
      if(msg.kind == SFTP_CtrlMsgKind_ThreadExit)
      {
        break;
      }
    }
    thread_join(control, max_U64);
  }
  if(connecting)
  {
    libssh2_exit();
  }

  return 0;
}
