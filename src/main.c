// Phase 3 console client: a real control-connection thread does password
// auth over SFTP and lists a remote directory, reporting results back to
// this (console) thread over a GuardedRing - the same architecture the
// eventual GUI will sit on top of, just printing to stdout for now instead
// of drawing a file list.
//
// Connection details come from the environment: FTP_HOST, FTP_PORT,
// FTP_USER, FTP_PASS.

#include "base/base_inc.h"
#include "base/base_inc.c"

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

int
main(void)
{
  os_init();

  TCTX *tctx = tctx_alloc();
  tctx_select(tctx);
  set_thread_name(str8_lit("main"));

  Arena *arena = arena_alloc();

  String8 host = env_str8(arena, "FTP_HOST");
  String8 user = env_str8(arena, "FTP_USER");
  String8 pass = env_str8(arena, "FTP_PASS");
  String8 port_str = env_str8(arena, "FTP_PORT");
  U16 port = port_str.size ? (U16)atoi((char *)port_str.str) : 22;
  if(port == 0)
  {
    port = 22;
  }

  if(host.size == 0 || user.size == 0 || pass.size == 0)
  {
    fprintf(stderr, "set FTP_HOST, FTP_USER, FTP_PASS (and optionally FTP_PORT) and try again\n");
    return 1;
  }

  libssh2_init(0);

  GuardedRing ring = guarded_ring_alloc(arena, KB(64));
  SFTP_ConnectParams params = {host, port, user, pass, str8_lit("."), &ring};
  U64 log_start_us = now_time_us();
  Thread control = thread_launch(sftp_control_thread_entry, &params);

  S32 exit_code = 0;
  for(;;)
  {
    SFTP_CtrlMsg msg = {0};
    guarded_ring_read_struct_or_wait(&ring, &msg, max_U64);
    F64 elapsed_s = (F64)(msg.timestamp_us - log_start_us) / 1000000.0;
    switch(msg.kind)
    {
      default: break;
      case SFTP_CtrlMsgKind_Log:
      {
        printf("[%7.3f] %.*s\n", elapsed_s, (int)msg.text_size, (char *)msg.text);
      }break;
      case SFTP_CtrlMsgKind_Entry:
      {
        printf("  %s  %10llu  %.*s\n",
               msg.is_dir ? "<DIR>" : "     ",
               msg.size_bytes,
               (int)msg.name_size, (char *)msg.name);
      }break;
      case SFTP_CtrlMsgKind_Error:
      {
        fprintf(stderr, "[%7.3f] error: %.*s\n", elapsed_s, (int)msg.text_size, (char *)msg.text);
        exit_code = 1;
      }break;
      case SFTP_CtrlMsgKind_Done:
      {
        goto listed;
      }break;
    }
  }
  listed:;

  thread_join(control, max_U64);
  libssh2_exit();
  arena_release(arena);
  tctx_release(tctx);
  return exit_code;
}
