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
              String8 display_name, String8 host, U16 port, String8 user, String8 pass,
              U8 host_key_hash[SFTP_HOST_KEY_HASH_SIZE], B32 ensure_parent_dir)
{
  if(q->op_count >= XFER_MAX_OPS)
  {
    // Silent before this fix - a large-enough folder download (one op per
    // file, no cap check in its own drain loop) could stop queuing files
    // partway through with zero indication anything was dropped. main.c
    // surfaces drop_count > 0 as a visible warning (see the Queue tab).
    q->drop_count += 1;
    return;
  }
  XFER_Op *op = &q->ops[q->op_count];
  MemoryZeroStruct(op);
  op->id = q->next_id;
  q->next_id += 1;
  op->kind = kind;
  op->local_path = str8_copy(q->arena, local_path);
  op->remote_path = str8_copy(q->arena, remote_path);
  op->display_name = str8_copy(q->arena, display_name);
  op->host = str8_copy(q->arena, host);
  op->port = port;
  op->user = str8_copy(q->arena, user);
  op->pass = str8_copy(q->arena, pass);
  MemoryCopy(op->host_key_hash, host_key_hash, SFTP_HOST_KEY_HASH_SIZE);
  op->status = XFER_Status_Queued;
  op->ensure_parent_dir = ensure_parent_dir;
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
xfer_cancel(XFER_Queue *q, U64 index)
{
  if(index >= q->op_count)
  {
    return;
  }
  XFER_Op *op = &q->ops[index];
  if(op->status == XFER_Status_Queued)
  {
    xfer_cancel_queued(q, index);
  }
  else if(op->status == XFER_Status_InProgress)
  {
    ins_atomic_u64_eval_assign(&op->cancel_requested, 1);
  }
}

internal void
xfer_cancel_by_id(XFER_Queue *q, U64 id)
{
  for(U64 i = 0; i < q->op_count; i += 1)
  {
    if(q->ops[i].id == id)
    {
      xfer_cancel(q, i);
      break;
    }
  }
}

internal void
xfer_resume(XFER_Queue *q, U64 index)
{
  if(index >= q->op_count)
  {
    return;
  }
  XFER_Op *op = &q->ops[index];
  if(op->status != XFER_Status_Failed)
  {
    return;
  }
  op->resume_requested = 1;
  op->cancel_requested = 0;
  op->error_text = (String8){0};
  op->speed_sample_count = 0;
  op->current_bps = 0.0;
  op->status = XFER_Status_Queued;
}

// Appends one (time,bytes) sample to the op's short speed window (shifting
// out the oldest once full - XFER_SPEED_SAMPLES is tiny, a shift is cheaper
// and far more obviously-correct than modular ring-index arithmetic here)
// and recomputes current_bps from oldest-to-newest in that window.
internal void
xfer__record_speed_sample(XFER_Op *op, U64 time_us, U64 bytes_done)
{
  if(op->speed_sample_count < XFER_SPEED_SAMPLES)
  {
    op->speed_sample_time_us[op->speed_sample_count] = time_us;
    op->speed_sample_bytes[op->speed_sample_count] = bytes_done;
    op->speed_sample_count += 1;
  }
  else
  {
    for(U32 i = 0; i + 1 < XFER_SPEED_SAMPLES; i += 1)
    {
      op->speed_sample_time_us[i] = op->speed_sample_time_us[i + 1];
      op->speed_sample_bytes[i] = op->speed_sample_bytes[i + 1];
    }
    op->speed_sample_time_us[XFER_SPEED_SAMPLES - 1] = time_us;
    op->speed_sample_bytes[XFER_SPEED_SAMPLES - 1] = bytes_done;
  }

  if(op->speed_sample_count >= 2)
  {
    U64 dt_us = op->speed_sample_time_us[op->speed_sample_count - 1] - op->speed_sample_time_us[0];
    U64 dbytes = op->speed_sample_bytes[op->speed_sample_count - 1] - op->speed_sample_bytes[0];
    if(dt_us > 0)
    {
      op->current_bps = (F64)dbytes * 1000000.0 / (F64)dt_us;
    }
  }
}

internal String8
xfer_format_speed(Arena *arena, F64 bytes_per_sec)
{
  String8 result;
  if(bytes_per_sec >= 1024.0 * 1024.0)
  {
    result = str8f(arena, "%.1f MB/s", bytes_per_sec / (1024.0 * 1024.0));
  }
  else if(bytes_per_sec >= 1024.0)
  {
    result = str8f(arena, "%.1f KB/s", bytes_per_sec / 1024.0);
  }
  else
  {
    result = str8f(arena, "%.0f B/s", bytes_per_sec);
  }
  return result;
}

// Formats remaining time as "12s" / "3m 05s" / "1h 12m", from bytes still
// to move and the same current_bps xfer_format_speed reads - "--" while
// that's not established yet (op just started - see current_bps's own
// "0 until at least 2 samples exist" comment) rather than a misleading 0s/inf.
internal String8
xfer_format_eta(Arena *arena, U64 bytes_remaining, F64 bytes_per_sec)
{
  String8 result;
  if(bytes_per_sec < 1.0)
  {
    result = str8_lit("--");
  }
  else
  {
    U64 total_s = (U64)((F64)bytes_remaining / bytes_per_sec + 0.5);
    U64 h = total_s / 3600;
    U64 m = (total_s % 3600) / 60;
    U64 s = total_s % 60;
    if(h > 0)      { result = str8f(arena, "%lluh %02llum", h, m); }
    else if(m > 0) { result = str8f(arena, "%llum %02llus", m, s); }
    else           { result = str8f(arena, "%llus", s); }
  }
  return result;
}

// Best-effort mkdir of every ancestor directory in `path_cstr`, parent
// first - used only for folder-drag uploads (op->ensure_parent_dir), where
// some of these directories are genuinely new. Mutates `path_cstr` in
// place (temporarily) rather than copying, so the caller must pass a
// buffer it owns and can still use afterward - xfer_thread_entry's own
// scratch-arena remote_cstr fits exactly. Errors are deliberately
// unchecked: libssh2_sftp_mkdir on an already-existing directory just
// fails harmlessly, and a real failure (permissions, bad path) surfaces
// properly a moment later when the caller's own sftp_open on the final
// file fails.
internal void
xfer__ensure_remote_parent_dirs(LIBSSH2_SFTP *sftp, char *path_cstr)
{
  for(char *p = path_cstr; *p != 0; p += 1)
  {
    if(*p == '/' && p != path_cstr)
    {
      *p = 0;
      libssh2_sftp_mkdir(sftp, path_cstr,
                          LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP |
                          LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH);
      *p = '/';
    }
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
    sftp_apply_hardened_algorithms(session);
    libssh2_session_set_timeout(session, SFTP_SESSION_TIMEOUT_MS);
    if(libssh2_session_handshake(session, (libssh2_socket_t)sock) != 0)
    {
      snprintf(err_buf, sizeof(err_buf), "SSH handshake failed");
      ok = 0;
    }
  }
  if(ok)
  {
    // Fail closed, no prompt: this thread never shows UI, and by the time
    // any transfer can be enqueued at all, the control connection has
    // already gone through sftp_client.c's TOFU/mismatch prompt for this
    // (host, port) - anything other than an exact match here means a MITM
    // sitting in front of *this specific* connection attempt, not a
    // legitimate first-time-ever endpoint. See XFER_Op::host_key_hash.
    U8 host_hash[SFTP_HOST_KEY_HASH_SIZE];
    if(!sftp_host_key_hash(session, host_hash) ||
       MemoryCompare(host_hash, op->host_key_hash, SFTP_HOST_KEY_HASH_SIZE) != 0)
    {
      snprintf(err_buf, sizeof(err_buf), "host key verification failed - possible network attack; "
                                          "reconnect via the main connection to re-verify");
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
  U64 resume_offset = 0;
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
        if(op->resume_requested)
        {
          // Ground truth is what's actually on disk locally, not our last
          // progress report - clamped to size_total so a remote file that
          // shrank/changed underneath us can't seek the read past its own EOF.
          U64 existing = 0;
          if(fs_file_size(op->local_path, &existing))
          {
            resume_offset = Min(existing, size_total);
          }
        }
        if(resume_offset > 0)
        {
          libssh2_sftp_seek64(remote_file, resume_offset);
        }
        if(op->ensure_parent_dir)
        {
          fs_dir_create(fs_path_parent(scratch.arena, op->local_path));
        }
        local_file = op->resume_requested ? fs_file_open_write_keep(op->local_path) : fs_file_open_write(op->local_path);
        if(!fs_file_is_valid(local_file))
        {
          snprintf(err_buf, sizeof(err_buf), "could not create local file");
          ok = 0;
        }
        else if(resume_offset > 0)
        {
          fs_file_seek(local_file, resume_offset);
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
        if(op->ensure_parent_dir)
        {
          xfer__ensure_remote_parent_dirs(sftp, remote_cstr);
        }
        U32 open_flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | (op->resume_requested ? 0 : LIBSSH2_FXF_TRUNC);
        remote_file = libssh2_sftp_open(sftp, remote_cstr, open_flags,
                                         LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                                         LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
        if(remote_file == 0)
        {
          snprintf(err_buf, sizeof(err_buf), "could not open remote destination");
          ok = 0;
        }
        else if(op->resume_requested)
        {
          // Ground truth is what actually landed remotely, not our last
          // progress report (see xfer_resume) - mirrors the vendored
          // third_party/libssh2/example/sftp_append.c resume pattern:
          // open without truncate, fstat the handle, seek to its size.
          LIBSSH2_SFTP_ATTRIBUTES attrs = {0};
          U64 existing = 0;
          if(libssh2_sftp_fstat(remote_file, &attrs) == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))
          {
            existing = attrs.filesize;
          }
          resume_offset = Min(existing, size_total);
          if(resume_offset > 0)
          {
            libssh2_sftp_seek64(remote_file, resume_offset);
            fs_file_seek(local_file, resume_offset);
          }
        }
      }
    }
  }

  if(ok)
  {
    XFER_Msg start_msg = {0};
    start_msg.kind = XFER_MsgKind_Progress;
    start_msg.bytes_done = resume_offset;
    start_msg.size_total = size_total;
    guarded_ring_write_struct_or_wait(&op->progress_ring, &start_msg, max_U64);

    U8 *chunk = push_array_no_zero(scratch.arena, U8, XFER_CHUNK_SIZE);
    U64 total_done = resume_offset;
    U64 since_last_report = 0;
    U64 last_report_time_us = now_time_us();
    for(;;)
    {
      if(ins_atomic_u64_eval(&op->cancel_requested))
      {
        ok = 0;
        snprintf(err_buf, sizeof(err_buf), "cancelled by user");
        break;
      }

      U64 chunk_got = 0;
      if(op->kind == XFER_Kind_Download)
      {
        // libssh2_sftp_read can return fewer bytes than requested even in
        // blocking mode (confirmed via header research) - loop until the
        // chunk buffer is full or EOF/error.
        while(ok && chunk_got < XFER_CHUNK_SIZE)
        {
          ssize_t rc = libssh2_sftp_read(remote_file, (char *)chunk + chunk_got, XFER_CHUNK_SIZE - chunk_got);
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
        chunk_got = fs_file_read(local_file, chunk, XFER_CHUNK_SIZE);
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
      U64 now = now_time_us();
      // Whichever comes first - 64KB moved, or 250ms elapsed - so a slow
      // transfer still gets fresh-enough samples for a responsive speed
      // figure rather than going quiet until the next 64KB boundary.
      if(since_last_report >= XFER_PROGRESS_REPORT_BYTES || (now - last_report_time_us) >= XFER_PROGRESS_REPORT_US)
      {
        since_last_report = 0;
        last_report_time_us = now;
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
          xfer__record_speed_sample(op, now_time_us(), op->bytes_done);
        }break;
        case XFER_MsgKind_Done:
        {
          op->bytes_done = msg.bytes_done;
          xfer__record_speed_sample(op, now_time_us(), op->bytes_done);
          op->status = XFER_Status_Done;
          thread_join(op->thread, max_U64);
          op->thread_joined = 1;
          // A Done op never resumes (only Failed ones can - xfer_resume),
          // so its password copy (see XFER_Op's own comment on why every
          // op carries one) has no further reason to exist. xfer_arena is
          // never cleared for the app's life, so without this a long
          // session with many transfers accumulates one resident
          // plaintext password per historical op indefinitely; zeroing it
          // in place is the best this arena allocator can do short of
          // actually freeing the bytes.
          MemorySet((void *)op->pass.str, 0, op->pass.size);
          op->pass.size = 0;
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

// Recursively walks `abs_dir` (reporting every file found, relative to the
// scan's remote_root, as `rel_prefix` + name), depth-first. Returns 0 (and
// leaves an error in err_buf) the moment anything goes wrong, unwinding
// without visiting whatever's left - a folder-drag download is a one-shot
// best-effort operation, same as any single transfer, so there's no
// disconnect/retry machinery here to mirror sftp_client.c's control-thread
// loop.
internal B32
xfer__scan_walk(XFER_Scan *scan, LIBSSH2_SFTP *sftp, String8 abs_dir, String8 rel_prefix,
                  char *err_buf, U64 err_buf_size)
{
  B32 ok = 1;
  Temp scratch = scratch_begin(0, 0);
  char *dir_cstr = str8_to_cstring(scratch.arena, abs_dir);
  LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, dir_cstr);
  if(dir == 0)
  {
    ok = 0;
    snprintf(err_buf, err_buf_size, "could not open remote directory '%s'", dir_cstr);
  }
  else
  {
    for(;;)
    {
      U8 name_buf[XFER_SCAN_PATH_MAX];
      LIBSSH2_SFTP_ATTRIBUTES attrs = {0};
      int rc = libssh2_sftp_readdir(dir, (char *)name_buf, sizeof(name_buf), &attrs);
      if(rc == 0) { break; }
      if(rc < 0)
      {
        ok = 0;
        snprintf(err_buf, err_buf_size, "error listing remote directory '%s'", dir_cstr);
        break;
      }
      String8 name = str8(name_buf, (U64)rc);
      if(str8_match(name, str8_lit(".")) || str8_match(name, str8_lit("..")))
      {
        continue;
      }
      if(!sftp_name_is_safe_path_component(name))
      {
        // A malicious or MITM-tampered server naming an entry e.g.
        // "..\..\..\Startup\evil.exe" would otherwise land exactly there
        // once child_rel below gets naively joined onto the local
        // download root (see xfer_scan_poll_all) - skip just this one
        // entry rather than aborting the whole folder download over it.
        continue;
      }

      Temp child_scratch = scratch_begin(0, 0);
      String8 child_abs = str8f(child_scratch.arena, "%.*s/%.*s",
                                 (int)abs_dir.size, (char *)abs_dir.str, (int)name.size, (char *)name.str);
      String8 child_rel = (rel_prefix.size == 0)
        ? str8_copy(child_scratch.arena, name)
        : str8f(child_scratch.arena, "%.*s/%.*s", (int)rel_prefix.size, (char *)rel_prefix.str, (int)name.size, (char *)name.str);

      B32 is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0 && LIBSSH2_SFTP_S_ISDIR(attrs.permissions) != 0;
      if(is_dir)
      {
        ok = xfer__scan_walk(scan, sftp, child_abs, child_rel, err_buf, err_buf_size);
      }
      else
      {
        XFER_ScanMsg msg = {0};
        msg.kind = XFER_ScanMsgKind_File;
        if(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) { msg.size_bytes = attrs.filesize; }
        msg.rel_path_size = Min(child_rel.size, sizeof(msg.rel_path) - 1);
        MemoryCopy(msg.rel_path, child_rel.str, msg.rel_path_size);
        guarded_ring_write_struct_or_wait(&scan->ring, &msg, max_U64);
      }
      scratch_end(child_scratch);
      if(!ok) { break; }
    }
    libssh2_sftp_closedir(dir);
  }
  scratch_end(scratch);
  return ok;
}

internal void
xfer_scan_thread_entry(void *ptr)
{
  XFER_Scan *scan = (XFER_Scan *)ptr;
  set_thread_name(str8_lit("xfer-scan"));

  Temp scratch = scratch_begin(0, 0);
  char *user_cstr = str8_to_cstring(scratch.arena, scan->user);
  char *pass_cstr = str8_to_cstring(scratch.arena, scan->pass);

  U64 sock = 0;
  LIBSSH2_SESSION *session = 0;
  LIBSSH2_SFTP *sftp = 0;
  B32 ok = 1;
  char err_buf[XFER_ERROR_MAX];
  err_buf[0] = 0;

  // Same independent connect/handshake/auth sequence xfer_thread_entry
  // duplicates from sftp_client.c - see that function's own comment for why.
  if(ok && !net_tcp_connect(scan->host, scan->port, &sock))
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
    sftp_apply_hardened_algorithms(session);
    libssh2_session_set_timeout(session, SFTP_SESSION_TIMEOUT_MS);
    if(libssh2_session_handshake(session, (libssh2_socket_t)sock) != 0)
    {
      snprintf(err_buf, sizeof(err_buf), "SSH handshake failed");
      ok = 0;
    }
  }
  if(ok)
  {
    // Fail closed, no prompt - see xfer_thread_entry's identical check for why.
    U8 host_hash[SFTP_HOST_KEY_HASH_SIZE];
    if(!sftp_host_key_hash(session, host_hash) ||
       MemoryCompare(host_hash, scan->host_key_hash, SFTP_HOST_KEY_HASH_SIZE) != 0)
    {
      snprintf(err_buf, sizeof(err_buf), "host key verification failed - possible network attack; "
                                          "reconnect via the main connection to re-verify");
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

  if(ok)
  {
    ok = xfer__scan_walk(scan, sftp, scan->remote_root, str8_lit(""), err_buf, sizeof(err_buf));
  }

  XFER_ScanMsg term_msg = {0};
  if(ok)
  {
    term_msg.kind = XFER_ScanMsgKind_Done;
  }
  else
  {
    term_msg.kind = XFER_ScanMsgKind_Failed;
    String8 err_str8 = str8_cstring(err_buf);
    term_msg.rel_path_size = Min(err_str8.size, sizeof(term_msg.rel_path) - 1);
    MemoryCopy(term_msg.rel_path, err_str8.str, term_msg.rel_path_size);
  }
  guarded_ring_write_struct_or_wait(&scan->ring, &term_msg, max_U64);

  if(sftp != 0) { libssh2_sftp_shutdown(sftp); }
  if(session != 0)
  {
    libssh2_session_disconnect(session, "scan done");
    libssh2_session_free(session);
  }
  if(sock != 0) { net_close(sock); }
  scratch_end(scratch);
}

internal void
xfer_scan_start(XFER_Scan *scans, U64 scan_count, Arena *arena,
                  String8 local_root, String8 remote_root,
                  String8 host, U16 port, String8 user, String8 pass,
                  U8 host_key_hash[SFTP_HOST_KEY_HASH_SIZE])
{
  for(U64 i = 0; i < scan_count; i += 1)
  {
    if(!scans[i].in_use)
    {
      XFER_Scan *s = &scans[i];
      MemoryZeroStruct(s);
      s->in_use = 1;
      s->local_root = str8_copy(arena, local_root);
      s->remote_root = str8_copy(arena, remote_root);
      s->host = str8_copy(arena, host);
      s->port = port;
      s->user = str8_copy(arena, user);
      s->pass = str8_copy(arena, pass);
      MemoryCopy(s->host_key_hash, host_key_hash, SFTP_HOST_KEY_HASH_SIZE);
      s->ring = guarded_ring_alloc(arena, KB(4));
      s->thread = thread_launch(xfer_scan_thread_entry, s);
      break;
    }
  }
}

internal void
xfer_scan_poll_all(XFER_Scan *scans, U64 scan_count, XFER_Queue *xfer_queue,
                     Arena *log_arena, String8List *log_lines)
{
  for(U64 i = 0; i < scan_count; i += 1)
  {
    XFER_Scan *s = &scans[i];
    if(!s->in_use)
    {
      continue;
    }
    for(;;)
    {
      XFER_ScanMsg msg = {0};
      if(!guarded_ring_read_struct_or_wait(&s->ring, &msg, now_time_us()))
      {
        break;
      }
      B32 slot_done = 0;
      switch(msg.kind)
      {
        default: break;
        case XFER_ScanMsgKind_File:
        {
          Temp scratch = scratch_begin(0, 0);
          String8 rel = str8(msg.rel_path, msg.rel_path_size);
          U8 *rel_win = push_array_no_zero(scratch.arena, U8, rel.size);
          for(U64 c = 0; c < rel.size; c += 1) { rel_win[c] = (rel.str[c] == '/') ? '\\' : rel.str[c]; }
          String8 local_full = str8f(scratch.arena, "%.*s\\%.*s",
                                      (int)s->local_root.size, (char *)s->local_root.str, (int)rel.size, (char *)rel_win);
          String8 remote_full = str8f(scratch.arena, "%.*s/%.*s",
                                       (int)s->remote_root.size, (char *)s->remote_root.str, (int)rel.size, (char *)rel.str);
          xfer_enqueue(xfer_queue, XFER_Kind_Download, local_full, remote_full, rel,
                       s->host, s->port, s->user, s->pass, s->host_key_hash, 1);
          scratch_end(scratch);
        }break;
        case XFER_ScanMsgKind_Done:
        {
          str8_list_push(log_arena, log_lines,
                          str8f(log_arena, "folder download of '%.*s' complete", (int)s->remote_root.size, (char *)s->remote_root.str));
          slot_done = 1;
        }break;
        case XFER_ScanMsgKind_Failed:
        {
          str8_list_push(log_arena, log_lines,
                          str8f(log_arena, "error: folder download of '%.*s' failed: %.*s",
                                (int)s->remote_root.size, (char *)s->remote_root.str,
                                (int)msg.rel_path_size, (char *)msg.rel_path));
          slot_done = 1;
        }break;
      }
      if(slot_done)
      {
        guarded_ring_release(&s->ring);
        thread_join(s->thread, max_U64);
        MemoryZeroStruct(s);
        break;
      }
    }
  }
}
