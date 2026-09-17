// Wire format between xfer_thread_entry and xfer_drain_progress - internal
// to this file, nothing outside needs to know about it.
typedef enum XFER_MsgKind
{
  XFER_MsgKind_Progress,
  XFER_MsgKind_Done,
  XFER_MsgKind_Failed,
}
XFER_MsgKind;

typedef struct XFER_Msg XFER_Msg;
struct XFER_Msg
{
  XFER_MsgKind kind;
  U64 bytes_done;
  U64 size_total; // sent on every Progress message (including the first, right after the size becomes known)
  U64 text_size;  // Failed only
  U8  text[XFER_ERROR_MAX];
};

internal void
xfer_queue_init(XFER_Queue *q, Arena *arena)
{
  MemoryZeroStruct(q);
  q->arena = arena;
}

internal void
xfer_enqueue(XFER_Queue *q, XFER_Kind kind, String8 local_path, String8 remote_path,
              String8 display_name, String8 host, U16 port, String8 user, String8 pass)
{
  if(q->op_count >= XFER_MAX_OPS)
  {
    return;
  }
  XFER_Op *op = &q->ops[q->op_count];
  MemoryZeroStruct(op);
  op->kind = kind;
  op->local_path = str8_copy(q->arena, local_path);
  op->remote_path = str8_copy(q->arena, remote_path);
  op->display_name = str8_copy(q->arena, display_name);
  op->host = str8_copy(q->arena, host);
  op->port = port;
  op->user = str8_copy(q->arena, user);
  op->pass = str8_copy(q->arena, pass);
  op->status = XFER_Status_Queued;
  op->progress_ring = guarded_ring_alloc(q->arena, KB(4));
  q->op_count += 1;
}

internal void
xfer_cancel_queued(XFER_Queue *q, U64 index)
{
  if(index < q->op_count && q->ops[index].status == XFER_Status_Queued)
  {
    guarded_ring_release(&q->ops[index].progress_ring);
    for(U64 i = index; i + 1 < q->op_count; i += 1)
    {
      q->ops[i] = q->ops[i + 1];
    }
    q->op_count -= 1;
  }
}

internal void
xfer_thread_entry(void *ptr)
{
  XFER_Op *op = (XFER_Op *)ptr;
  set_thread_name(op->kind == XFER_Kind_Download ? str8_lit("xfer-download") : str8_lit("xfer-upload"));

  Temp scratch = scratch_begin(0, 0);
  char *user_cstr   = str8_to_cstring(scratch.arena, op->user);
  char *pass_cstr   = str8_to_cstring(scratch.arena, op->pass);
  char *remote_cstr = str8_to_cstring(scratch.arena, op->remote_path);

  U64 sock = 0;
  LIBSSH2_SESSION *session = 0;
  LIBSSH2_SFTP *sftp = 0;
  LIBSSH2_SFTP_HANDLE *remote_file = 0;
  FS_File local_file = {0};
  B32 ok = 1;
  char err_buf[XFER_ERROR_MAX];
  err_buf[0] = 0;

  if(ok && !net_tcp_connect(op->host, op->port, &sock))
  {
    snprintf(err_buf, sizeof(err_buf), "could not connect to server");
    ok = 0;
  }
  if(ok)
  {
    session = libssh2_session_init();
    if(session == 0)
    {
      snprintf(err_buf, sizeof(err_buf), "session init failed");
      ok = 0;
    }
  }
  if(ok)
  {
    libssh2_session_set_blocking(session, 1);
    if(libssh2_session_handshake(session, (libssh2_socket_t)sock) != 0)
    {
      snprintf(err_buf, sizeof(err_buf), "SSH handshake failed");
      ok = 0;
    }
  }
  if(ok && libssh2_userauth_password(session, user_cstr, pass_cstr) != 0)
  {
    snprintf(err_buf, sizeof(err_buf), "authentication failed");
    ok = 0;
  }
  if(ok)
  {
    sftp = libssh2_sftp_init(session);
    if(sftp == 0)
    {
      snprintf(err_buf, sizeof(err_buf), "SFTP init failed");
      ok = 0;
    }
  }

  U64 size_total = 0;
  if(ok)
  {
    if(op->kind == XFER_Kind_Download)
    {
      remote_file = libssh2_sftp_open(sftp, remote_cstr, LIBSSH2_FXF_READ, 0);
      if(remote_file == 0)
      {
        snprintf(err_buf, sizeof(err_buf), "could not open remote file");
        ok = 0;
      }
      else
      {
        LIBSSH2_SFTP_ATTRIBUTES attrs = {0};
        if(libssh2_sftp_fstat(remote_file, &attrs) == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))
        {
          size_total = attrs.filesize;
        }
        local_file = fs_file_open_write(op->local_path);
        if(!fs_file_is_valid(local_file))
        {
          snprintf(err_buf, sizeof(err_buf), "could not create local file");
          ok = 0;
        }
      }
    }
    else // Upload
    {
      local_file = fs_file_open_read(op->local_path);
      if(!fs_file_is_valid(local_file))
      {
        snprintf(err_buf, sizeof(err_buf), "could not open local file");
        ok = 0;
      }
      else
      {
        fs_file_size(op->local_path, &size_total);
        remote_file = libssh2_sftp_open(sftp, remote_cstr,
                                         LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                         LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                                         LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
        if(remote_file == 0)
        {
          snprintf(err_buf, sizeof(err_buf), "could not open remote destination");
          ok = 0;
        }
      }
    }
  }

  if(ok)
  {
    XFER_Msg start_msg = {0};
    start_msg.kind = XFER_MsgKind_Progress;
    start_msg.size_total = size_total;
    guarded_ring_write_struct_or_wait(&op->progress_ring, &start_msg, max_U64);

    U8 chunk[XFER_CHUNK_SIZE];
    U64 total_done = 0;
    U64 since_last_report = 0;
    for(;;)
    {
      U64 chunk_got = 0;
      if(op->kind == XFER_Kind_Download)
      {
        // libssh2_sftp_read can return fewer bytes than requested even in
        // blocking mode (confirmed via header research) - loop until the
        // chunk buffer is full or EOF/error.
        while(ok && chunk_got < sizeof(chunk))
        {
          ssize_t rc = libssh2_sftp_read(remote_file, (char *)chunk + chunk_got, sizeof(chunk) - chunk_got);
          if(rc == 0) { break; } // EOF
          if(rc < 0)
          {
            ok = 0;
            snprintf(err_buf, sizeof(err_buf), "read failed during transfer");
            break;
          }
          chunk_got += (U64)rc;
        }
        if(ok && chunk_got > 0 && !fs_file_write(local_file, chunk, chunk_got))
        {
          ok = 0;
          snprintf(err_buf, sizeof(err_buf), "local write failed (disk full?)");
        }
      }
      else // Upload
      {
        chunk_got = fs_file_read(local_file, chunk, sizeof(chunk));
        if(chunk_got > 0)
        {
          // libssh2_sftp_write may accept fewer bytes than requested - loop
          // until the whole chunk is actually sent.
          U64 sent = 0;
          while(ok && sent < chunk_got)
          {
            ssize_t rc = libssh2_sftp_write(remote_file, (char *)chunk + sent, chunk_got - sent);
            if(rc < 0)
            {
              ok = 0;
              snprintf(err_buf, sizeof(err_buf), "remote write failed");
              break;
            }
            sent += (U64)rc;
          }
        }
      }

      if(!ok || chunk_got == 0)
      {
        break;
      }

      total_done += chunk_got;
      since_last_report += chunk_got;
      if(since_last_report >= XFER_PROGRESS_REPORT_BYTES)
      {
        since_last_report = 0;
        XFER_Msg progress_msg = {0};
        progress_msg.kind = XFER_MsgKind_Progress;
        progress_msg.bytes_done = total_done;
        progress_msg.size_total = size_total;
        guarded_ring_write_struct_or_wait(&op->progress_ring, &progress_msg, max_U64);
      }
    }

    if(ok)
    {
      XFER_Msg done_msg = {0};
      done_msg.kind = XFER_MsgKind_Done;
      done_msg.bytes_done = total_done;
      done_msg.size_total = size_total;
      guarded_ring_write_struct_or_wait(&op->progress_ring, &done_msg, max_U64);
    }
  }

  if(!ok)
  {
    XFER_Msg fail_msg = {0};
    fail_msg.kind = XFER_MsgKind_Failed;
    String8 err_str8 = str8_cstring(err_buf);
    fail_msg.text_size = Min(err_str8.size, sizeof(fail_msg.text) - 1);
    MemoryCopy(fail_msg.text, err_str8.str, fail_msg.text_size);
    guarded_ring_write_struct_or_wait(&op->progress_ring, &fail_msg, max_U64);
  }

  if(remote_file != 0) { libssh2_sftp_close_handle(remote_file); }
  if(fs_file_is_valid(local_file)) { fs_file_close(local_file); }
  if(sftp != 0) { libssh2_sftp_shutdown(sftp); }
  if(session != 0)
  {
    libssh2_session_disconnect(session, "transfer done");
    libssh2_session_free(session);
  }
  if(sock != 0) { net_close(sock); }
  scratch_end(scratch);
}

internal void
xfer_dispatch(XFER_Queue *q, U32 max_downloads, U32 max_uploads)
{
  U32 active_downloads = 0, active_uploads = 0;
  for(U64 i = 0; i < q->op_count; i += 1)
  {
    if(q->ops[i].status == XFER_Status_InProgress)
    {
      if(q->ops[i].kind == XFER_Kind_Download) { active_downloads += 1; }
      else                                     { active_uploads += 1; }
    }
  }

  for(U64 i = 0; i < q->op_count; i += 1)
  {
    XFER_Op *op = &q->ops[i];
    if(op->status != XFER_Status_Queued)
    {
      continue;
    }
    if(op->kind == XFER_Kind_Download)
    {
      if(active_downloads >= max_downloads) { continue; }
      active_downloads += 1;
    }
    else
    {
      if(active_uploads >= max_uploads) { continue; }
      active_uploads += 1;
    }
    op->status = XFER_Status_InProgress;
    op->thread = thread_launch(xfer_thread_entry, op);
    op->thread_joined = 0;
  }
}

internal void
xfer_drain_progress(XFER_Queue *q)
{
  for(U64 i = 0; i < q->op_count; i += 1)
  {
    XFER_Op *op = &q->ops[i];
    if(op->status != XFER_Status_InProgress)
    {
      continue;
    }
    for(;;)
    {
      XFER_Msg msg = {0};
      if(!guarded_ring_read_struct_or_wait(&op->progress_ring, &msg, now_time_us()))
      {
        break;
      }
      if(msg.size_total > 0)
      {
        op->size_total = msg.size_total;
      }
      switch(msg.kind)
      {
        default: break;
        case XFER_MsgKind_Progress:
        {
          op->bytes_done = msg.bytes_done;
        }break;
        case XFER_MsgKind_Done:
        {
          op->bytes_done = msg.bytes_done;
          op->status = XFER_Status_Done;
          thread_join(op->thread, max_U64);
          op->thread_joined = 1;
        }break;
        case XFER_MsgKind_Failed:
        {
          op->error_text = str8_copy(q->arena, str8(msg.text, msg.text_size));
          op->status = XFER_Status_Failed;
          thread_join(op->thread, max_U64);
          op->thread_joined = 1;
        }break;
      }
      if(op->thread_joined)
      {
        break;
      }
    }
  }
}
