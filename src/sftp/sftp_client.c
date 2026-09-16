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

internal void
sftp_control_thread_entry(void *ptr)
{
  SFTP_ConnectParams *params = (SFTP_ConnectParams *)ptr;
  GuardedRing *ring = params->out_ring;
  set_thread_name(str8_lit("sftp-control"));

  Temp scratch = scratch_begin(0, 0);
  char *host_cstr = str8_to_cstring(scratch.arena, params->host);
  char *user_cstr = str8_to_cstring(scratch.arena, params->username);
  char *pass_cstr = str8_to_cstring(scratch.arena, params->password);
  char *dir_cstr  = str8_to_cstring(scratch.arena, params->remote_dir);

  U64 sock = 0;
  LIBSSH2_SESSION *session = 0;
  LIBSSH2_SFTP *sftp = 0;

  sftp_log(ring, "connecting to %s:%u", host_cstr, (U32)params->port);
  if(!net_tcp_connect(params->host, params->port, &sock))
  {
    sftp_send_error(ring, "could not connect to %s:%u", host_cstr, (U32)params->port);
    goto done;
  }
  sftp_log(ring, "connected");

  session = libssh2_session_init();
  if(session == 0)
  {
    sftp_send_error(ring, "libssh2_session_init failed");
    net_close(sock);
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
    goto done;
  }

  {
    sftp_log(ring, "opening directory '%s'", dir_cstr);
    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, dir_cstr);
    if(dir == 0)
    {
      sftp_send_error(ring, "could not open remote directory '%s'", dir_cstr);
    }
    else
    {
      U64 entry_count = 0;
      for(;;)
      {
        U8 name_buf[SFTP_NAME_MAX];
        LIBSSH2_SFTP_ATTRIBUTES attrs = {0};
        int rc = libssh2_sftp_readdir(dir, (char *)name_buf, sizeof(name_buf), &attrs);
        if(rc <= 0)
        {
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
      sftp_log(ring, "directory listing complete (%llu entries)", entry_count);
    }
  }

  sftp_log(ring, "closing connection");
  libssh2_sftp_shutdown(sftp);
  libssh2_session_disconnect(session, "done");
  libssh2_session_free(session);
  net_close(sock);

  done:;
  scratch_end(scratch);
  SFTP_CtrlMsg done_msg = {0};
  done_msg.kind = SFTP_CtrlMsgKind_Done;
  guarded_ring_write_struct_or_wait(ring, &done_msg, max_U64);
}
