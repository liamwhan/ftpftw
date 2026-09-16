internal void
sftp_send_msg(GuardedRing *ring, SFTP_CtrlMsgKind kind, char *fmt, va_list args)
{
  Temp scratch = scratch_begin(0, 0);

  String8 text = str8fv(scratch.arena, fmt, args);

  SFTP_CtrlMsg msg = {0};
  msg.kind = kind;
  msg.timestamp_us = now_time_us();
  msg.text_size = Min(text.size, sizeof(msg.text) - 1);
  MemoryCopy(msg.text, text.str, msg.text_size);
  guarded_ring_write_struct_or_wait(ring, &msg, max_U64);

  scratch_end(scratch);
}

internal void
sftp_log(GuardedRing *ring, char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  sftp_send_msg(ring, SFTP_CtrlMsgKind_Log, fmt, args);
  va_end(args);
}

internal void
sftp_send_error(GuardedRing *ring, char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  sftp_send_msg(ring, SFTP_CtrlMsgKind_Error, fmt, args);
  va_end(args);
}

// A failure from an SFTP call is either the server legitimately saying no
// (bad path, permissions - LIBSSH2_ERROR_SFTP_PROTOCOL, session is fine)
// or the connection itself being dead (anything else - socket/transport
// level). Only the latter should trigger a reconnect.
internal B32
sftp_is_disconnect_errno(int errnum)
{
  return errnum != LIBSSH2_ERROR_NONE && errnum != LIBSSH2_ERROR_SFTP_PROTOCOL;
}

// One listing operation: open `path`, stream its entries as Entry
// messages, close, and send the terminal Done for this listing - unless
// the connection turns out to be dead, in which case no Done is sent
// (misleading to claim an operation "completed" right before the whole
// session drops) and this returns 1 so the caller reconnects instead of
// looping for the next request. Used both for the initial connect-time
// listing and every subsequent on-demand one.
internal B32
sftp_do_list(GuardedRing *ring, LIBSSH2_SESSION *session, LIBSSH2_SFTP *sftp, char *path_cstr)
{
  B32 disconnected = 0;

  sftp_log(ring, "opening directory '%s'", path_cstr);
  LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, path_cstr);
  if(dir == 0)
  {
    int errnum = libssh2_session_last_errno(session);
    if(sftp_is_disconnect_errno(errnum))
    {
      disconnected = 1;
      sftp_log(ring, "connection lost while opening '%s' (libssh2 error %d)", path_cstr, errnum);
    }
    else
    {
      sftp_send_error(ring, "could not open remote directory '%s'", path_cstr);
    }
  }
  else
  {
    U64 entry_count = 0;
    for(;;)
    {
      U8 name_buf[SFTP_NAME_MAX];
      LIBSSH2_SFTP_ATTRIBUTES attrs = {0};
      int rc = libssh2_sftp_readdir(dir, (char *)name_buf, sizeof(name_buf), &attrs);
      if(rc == 0)
      {
        break;
      }
      if(rc < 0)
      {
        int errnum = libssh2_session_last_errno(session);
        if(sftp_is_disconnect_errno(errnum))
        {
          disconnected = 1;
          sftp_log(ring, "connection lost while listing '%s' (libssh2 error %d)", path_cstr, errnum);
        }
        break;
      }
      SFTP_CtrlMsg msg = {0};
      msg.kind = SFTP_CtrlMsgKind_Entry;
      msg.name_size = Min((U64)rc, sizeof(msg.name) - 1);
      MemoryCopy(msg.name, name_buf, msg.name_size);
      if(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
      {
        msg.size_bytes = attrs.filesize;
      }
      if(attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
      {
        msg.is_dir = LIBSSH2_SFTP_S_ISDIR(attrs.permissions) != 0;
      }
      guarded_ring_write_struct_or_wait(ring, &msg, max_U64);
      entry_count += 1;
    }
    libssh2_sftp_closedir(dir);
    if(!disconnected)
    {
      sftp_log(ring, "directory listing complete (%llu entries)", entry_count);
    }
  }

  if(!disconnected)
  {
    SFTP_CtrlMsg done_msg = {0};
    done_msg.kind = SFTP_CtrlMsgKind_Done;
    guarded_ring_write_struct_or_wait(ring, &done_msg, max_U64);
  }
  return disconnected;
}

internal void
sftp_control_thread_entry(void *ptr)
{
  SFTP_ConnectParams *params = (SFTP_ConnectParams *)ptr;
  GuardedRing *ring = params->out_ring;
  GuardedRing *in_ring = params->in_ring;
  set_thread_name(str8_lit("sftp-control"));

  Temp scratch = scratch_begin(0, 0);
  char *host_cstr = str8_to_cstring(scratch.arena, params->host);
  char *user_cstr = str8_to_cstring(scratch.arena, params->username);
  char *pass_cstr = str8_to_cstring(scratch.arena, params->password);
  char *dir_cstr  = str8_to_cstring(scratch.arena, params->remote_dir);

  U64 sock = 0;
  LIBSSH2_SESSION *session = 0;
  LIBSSH2_SFTP *sftp = 0;
  // Whether this thread is exiting because the connection died (caller
  // should reconnect) vs. a deliberate Quit or bad credentials (caller
  // should not). Everything before a successful auth is treated as
  // retryable (could be a transient network/DNS blip); only bad
  // credentials specifically is not, since retrying with the same
  // password will never succeed.
  B32 lost_connection = 0;

  sftp_log(ring, "connecting to %s:%u", host_cstr, (U32)params->port);
  if(!net_tcp_connect(params->host, params->port, &sock))
  {
    sftp_send_error(ring, "could not connect to %s:%u", host_cstr, (U32)params->port);
    lost_connection = 1;
    goto done;
  }
  sftp_log(ring, "connected");

  session = libssh2_session_init();
  if(session == 0)
  {
    sftp_send_error(ring, "libssh2_session_init failed");
    net_close(sock);
    lost_connection = 1;
    goto done;
  }
  libssh2_session_set_blocking(session, 1);

  sftp_log(ring, "starting SSH handshake");
  if(libssh2_session_handshake(session, (libssh2_socket_t)sock) != 0)
  {
    char *errmsg = 0;
    int errmsg_len = 0;
    libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
    sftp_send_error(ring, "SSH handshake failed: %.*s", errmsg_len, errmsg);
    libssh2_session_free(session);
    net_close(sock);
    lost_connection = 1;
    goto done;
  }
  sftp_log(ring, "SSH handshake complete");

  sftp_log(ring, "authenticating as '%s'", user_cstr);
  if(libssh2_userauth_password(session, user_cstr, pass_cstr) != 0)
  {
    char *errmsg = 0;
    int errmsg_len = 0;
    libssh2_session_last_error(session, &errmsg, &errmsg_len, 0);
    sftp_send_error(ring, "authentication failed: %.*s", errmsg_len, errmsg);
    libssh2_session_disconnect(session, "auth failed");
    libssh2_session_free(session);
    net_close(sock);
    // lost_connection stays 0 - bad credentials, not worth retrying.
    goto done;
  }
  sftp_log(ring, "authenticated");

  sftp_log(ring, "opening SFTP session");
  sftp = libssh2_sftp_init(session);
  if(sftp == 0)
  {
    sftp_send_error(ring, "libssh2_sftp_init failed");
    libssh2_session_disconnect(session, "sftp init failed");
    libssh2_session_free(session);
    net_close(sock);
    lost_connection = 1;
    goto done;
  }

  if(sftp_do_list(ring, session, sftp, dir_cstr))
  {
    lost_connection = 1;
    goto cleanup;
  }

  // Stay connected and service on-demand listing requests (remote pane
  // navigation) until asked to quit or the connection dies mid-request.
  for(;;)
  {
    SFTP_Req req = {0};
    guarded_ring_read_struct_or_wait(in_ring, &req, max_U64);
    if(req.kind == SFTP_ReqKind_Quit)
    {
      break;
    }
    else if(req.kind == SFTP_ReqKind_ListDir)
    {
      Temp req_scratch = scratch_begin(0, 0);
      String8 path = str8(req.path, Min(req.path_size, sizeof(req.path)));
      char *path_cstr = str8_to_cstring(req_scratch.arena, path);
      B32 disc = sftp_do_list(ring, session, sftp, path_cstr);
      scratch_end(req_scratch);
      if(disc)
      {
        lost_connection = 1;
        break;
      }
    }
  }

  cleanup:;
  sftp_log(ring, "closing connection");
  libssh2_sftp_shutdown(sftp);
  libssh2_session_disconnect(session, "done");
  libssh2_session_free(session);
  net_close(sock);

  done:;
  scratch_end(scratch);
  SFTP_CtrlMsg exit_msg = {0};
  exit_msg.kind = lost_connection ? SFTP_CtrlMsgKind_Disconnected : SFTP_CtrlMsgKind_ThreadExit;
  guarded_ring_write_struct_or_wait(ring, &exit_msg, max_U64);
}

internal String8
sftp_path_join(Arena *arena, String8 dir, String8 name)
{
  String8 result;
  if(str8_match(dir, str8_lit(".")))
  {
    result = str8_copy(arena, name);
  }
  else
  {
    B32 needs_sep = (dir.size > 0 && dir.str[dir.size - 1] != '/');
    result = str8f(arena, "%.*s%s%.*s",
                    (int)dir.size, (char *)dir.str,
                    needs_sep ? "/" : "",
                    (int)name.size, (char *)name.str);
  }
  return result;
}

internal String8
sftp_path_parent(Arena *arena, String8 path)
{
  String8 result;
  if(str8_match(path, str8_lit(".")))
  {
    result = str8_copy(arena, path); // already at the top - nowhere to go
  }
  else
  {
    S64 slash_idx = -1;
    for(S64 i = (S64)path.size - 1; i >= 0; i -= 1)
    {
      if(path.str[i] == '/')
      {
        slash_idx = i;
        break;
      }
    }
    result = (slash_idx < 0) ? str8_copy(arena, str8_lit("."))
                              : str8_copy(arena, str8(path.str, (U64)slash_idx));
  }
  return result;
}
