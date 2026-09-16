// GUI phase, milestone 2: the same connect -> directory-listing flow as the
// phase-3 console client, now rendered live in the window (milestone 1)
// instead of printed to stdout. sftp_client.h/.c are unchanged - this is
// just a second consumer of its GuardedRing protocol; the control thread
// doesn't know or care who's listening.

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

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "sftp/sftp_client.h"
#include "sftp/sftp_client.c"

internal String8
env_str8(Arena *arena, char *name)
{
  char *value = getenv(name);
  return value ? str8_cstring_copy(arena, value) : (String8){0};
}

// Same line formatting the console version used - just returned as a
// String8 instead of printf'd, so the draw layer can render it.
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
    case SFTP_CtrlMsgKind_Entry:
    {
      result = str8f(arena, "  %s  %10llu  %.*s",
                     msg->is_dir ? "<DIR>" : "     ",
                     msg->size_bytes,
                     (int)msg->name_size, (char *)msg->name);
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
  WM_Window window = wm_window_open(str8_lit("lwftpclient"), 900, 600);

  r_init();
  r_window_equip(window);

  fp_init();
  fnt_init();
  FP_Font font = fp_font_open("Segoe UI");

  Arena *arena = arena_alloc(); // app-lifetime: connect params, accumulated lines
  Arena *frame_arena = arena_alloc();

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

  GuardedRing ring = {0};
  SFTP_ConnectParams params = {0};
  Thread control = {0};
  B32 control_joined = 1;
  U64 log_start_us = 0;
  if(connecting)
  {
    libssh2_init(0);
    ring = guarded_ring_alloc(arena, KB(64));
    params.host = host;
    params.port = port;
    params.username = user;
    params.password = pass;
    params.remote_dir = str8_lit(".");
    params.out_ring = &ring;
    log_start_us = now_time_us();
    control = thread_launch(sftp_control_thread_entry, &params);
    control_joined = 0;
  }

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

    // Drain whatever the control thread has produced since last frame -
    // non-blocking: an already-elapsed deadline makes this a single poll
    // per message rather than a wait, so the render loop never stalls on
    // network I/O happening on the other thread.
    if(connecting && !control_joined)
    {
      for(;;)
      {
        SFTP_CtrlMsg msg = {0};
        if(!guarded_ring_read_struct_or_wait(&ring, &msg, now_time_us()))
        {
          break;
        }
        String8 line = line_from_ctrl_msg(arena, &msg, log_start_us);
        if(line.size != 0)
        {
          str8_list_push(arena, &lines, line);
        }
        if(msg.kind == SFTP_CtrlMsgKind_Done)
        {
          thread_join(control, max_U64);
          control_joined = 1;
          break;
        }
      }
    }

    dr_begin_frame(0.10f, 0.10f, 0.12f, 1.0f);
    F32 y = 20.0f;
    for(String8Node *n = lines.first; n != 0; n = n->next)
    {
      dr_text(&font, 18.0f, 20.0f, y, 0.85f, 0.85f, 0.85f, 1.0f, n->string);
      y += 22.0f;
    }
    dr_end_frame();
  }

  if(connecting && !control_joined)
  {
    thread_join(control, max_U64);
  }
  if(connecting)
  {
    libssh2_exit();
  }

  return 0;
}
