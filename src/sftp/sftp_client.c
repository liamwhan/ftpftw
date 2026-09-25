// Restricts KEX/hostkey/cipher/MAC negotiation to modern algorithms,
// removing legacy/weak options (SHA-1-based KEX, ssh-rsa/ssh-dss host
// keys, RC4/3DES/CBC ciphers, MD5/SHA-1 MACs) a downgrade-capable
// attacker could otherwise offer - the client would otherwise silently
// accept whatever libssh2's own defaults include. The exact list is what
// this vendored libssh2 build actually has compiled in for the WinCNG
// backend: LIBSSH2_ECDSA_WINCNG isn't defined (see build.bat), so
// curve25519/ECDH/ECDSA/Ed25519 are all unavailable here - the strongest
// KEX this build can do is finite-field DH with 2048-8192 bit MODP
// groups and SHA256/512, which is still solid. Applied identically by
// every thread that opens its own session - control (below), transfer,
// and scan (xfer.c).
internal void
sftp_apply_hardened_algorithms(LIBSSH2_SESSION *session)
{
  libssh2_session_method_pref(session, LIBSSH2_METHOD_KEX,
    "diffie-hellman-group18-sha512,diffie-hellman-group16-sha512,"
    "diffie-hellman-group-exchange-sha256,diffie-hellman-group14-sha256");
  libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY, "rsa-sha2-512,rsa-sha2-256");
  libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS,
    "chacha20-poly1305@openssh.com,aes256-ctr,aes192-ctr,aes128-ctr");
  libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC,
    "chacha20-poly1305@openssh.com,aes256-ctr,aes192-ctr,aes128-ctr");
  libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_CS,
    "hmac-sha2-512-etm@openssh.com,hmac-sha2-256-etm@openssh.com,hmac-sha2-512,hmac-sha2-256");
  libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_SC,
    "hmac-sha2-512-etm@openssh.com,hmac-sha2-256-etm@openssh.com,hmac-sha2-512,hmac-sha2-256");
}

// Bounds every blocking libssh2 call (handshake/auth/sftp ops) against a
// stalled/black-holed peer - without this, a thread stuck inside one
// blocking call never notices a Quit request until the OS's own (far
// longer, sometimes minutes) TCP-level timeout fires, freezing whatever's
// waiting on it (see main.c's teardown_live_session). Separate from, and
// in addition to, net_tcp_connect's own connect-time bound.
#define SFTP_SESSION_TIMEOUT_MS 20000

// Computes the just-negotiated session's host key SHA256 hash. Returns 0
// if the crypto backend couldn't produce one (shouldn't happen after a
// real completed handshake, but this guards a security decision - fail
// closed rather than silently skip the check) and leaves out_hash
// untouched.
internal B32
sftp_host_key_hash(LIBSSH2_SESSION *session, U8 out_hash[SFTP_HOST_KEY_HASH_SIZE])
{
  const char *hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
  B32 ok = (hash != 0);
  if(ok)
  {
    MemoryCopy(out_hash, hash, SFTP_HOST_KEY_HASH_SIZE);
  }
  return ok;
}

// "xx:xx:...:xx" colon-grouped hex - the traditional SSH fingerprint
// display format, 32 bytes -> 95 characters. Used for the host-key
// confirmation prompt (see main.c).
internal String8
sftp_format_host_key_hash(Arena *arena, U8 hash[SFTP_HOST_KEY_HASH_SIZE])
{
  local_persist char hex_digits[] = "0123456789abcdef";
  U8 *buf = push_array_no_zero(arena, U8, SFTP_HOST_KEY_HASH_SIZE * 3 - 1);
  U64 pos = 0;
  for(U64 i = 0; i < SFTP_HOST_KEY_HASH_SIZE; i += 1)
  {
    if(i != 0)
    {
      buf[pos] = ':';
      pos += 1;
    }
    buf[pos + 0] = hex_digits[(hash[i] >> 4) & 0xF];
    buf[pos + 1] = hex_digits[hash[i] & 0xF];
    pos += 2;
  }
  return str8(buf, pos);
}

// Rejects a server-supplied name (an SFTP directory-entry name from
// readdir) that isn't safe to use as a single path component when
// building a LOCAL filesystem path. "." and ".." are handled by each
// call site's own existing skip/navigate logic and aren't rejected
// here - this only blocks the characters that let one path component
// escape the directory it's being joined into: '/' or '\' (a fake
// "sibling" path component, e.g. "..\..\..\Startup\evil.exe"), ':' (an
// NTFS alternate-data-stream separator, or a drive letter), and an
// embedded NUL. Without this, a malicious or MITM-tampered server could
// plant such a name and have it land outside the chosen download
// directory once naively joined by fs_path_join (which does no
// validation of its own) - see main.c's drag-drop download handling and
// xfer.c's folder-scan download, the two places this matters.
internal B32
sftp_name_is_safe_path_component(String8 name)
{
  B32 safe = (name.size != 0);
  for(U64 i = 0; safe && i < name.size; i += 1)
  {
    U8 c = name.str[i];
    if(c == '/' || c == '\\' || c == ':' || c == 0)
    {
      safe = 0;
    }
  }
  return safe;
}

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

// Deletes a single remote file. Same disconnect-vs-protocol-error split as
// sftp_do_list: a dead connection returns 1 so the caller reconnects; a
// legitimate server-side no (bad path, permissions) just reports an Error
// and the thread keeps running.
internal B32
sftp_do_delete(GuardedRing *ring, LIBSSH2_SESSION *session, LIBSSH2_SFTP *sftp, char *path_cstr)
{
  B32 disconnected = 0;
  sftp_log(ring, "deleting '%s'", path_cstr);
  if(libssh2_sftp_unlink(sftp, path_cstr) != 0)
  {
    int errnum = libssh2_session_last_errno(session);
    if(sftp_is_disconnect_errno(errnum))
    {
      disconnected = 1;
      sftp_log(ring, "connection lost while deleting '%s' (libssh2 error %d)", path_cstr, errnum);
    }
    else
    {
      sftp_send_error(ring, "could not delete '%s'", path_cstr);
    }
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
  sftp_apply_hardened_algorithms(session);
  libssh2_session_set_timeout(session, SFTP_SESSION_TIMEOUT_MS);

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

  // Host key verification - must happen before any credential goes over
  // the wire (userauth_password, right below), otherwise a MITM sitting
  // in front of an unverified "server" would receive the plaintext
  // password before the user ever gets a chance to notice anything's
  // wrong. Silent when it matches what's already pinned for this
  // (host, port); otherwise blocks for a human decision - see
  // SFTP_CtrlMsgKind_HostKeyVerify's own comment in sftp_client.h.
  {
    U8 host_hash[SFTP_HOST_KEY_HASH_SIZE];
    if(!sftp_host_key_hash(session, host_hash))
    {
      sftp_send_error(ring, "could not verify host key (SHA256 hash unavailable) - refusing to connect");
      libssh2_session_disconnect(session, "host key verification unavailable");
      libssh2_session_free(session);
      net_close(sock);
      // lost_connection stays 0 - not a transient network problem, retrying won't fix it.
      goto done;
    }

    B32 already_trusted = params->has_pinned_host_key_hash &&
                          MemoryCompare(host_hash, params->pinned_host_key_hash, SFTP_HOST_KEY_HASH_SIZE) == 0;
    if(!already_trusted)
    {
      SFTP_CtrlMsg verify_msg = {0};
      verify_msg.kind = SFTP_CtrlMsgKind_HostKeyVerify;
      MemoryCopy(verify_msg.host_key_hash, host_hash, SFTP_HOST_KEY_HASH_SIZE);
      verify_msg.host_key_is_change = params->has_pinned_host_key_hash; // 1 = mismatch, 0 = first time ever
      guarded_ring_write_struct_or_wait(ring, &verify_msg, max_U64);

      B32 accepted = 0;
      for(B32 decided = 0; !decided;)
      {
        SFTP_Req req = {0};
        guarded_ring_read_struct_or_wait(in_ring, &req, max_U64); // a human decision, not network I/O - no timeout
        if(req.kind == SFTP_ReqKind_HostKeyAccept) { accepted = 1; decided = 1; }
        else if(req.kind == SFTP_ReqKind_HostKeyReject) { accepted = 0; decided = 1; }
        else if(req.kind == SFTP_ReqKind_Quit) { accepted = 0; decided = 1; }
        // Anything else can't legitimately arrive yet - the caller only
        // enables navigation/delete once past this gate. Ignore and keep
        // waiting rather than treat an unexpected message as a decision.
      }
      if(!accepted)
      {
        sftp_send_error(ring, "host key not trusted - connection aborted");
        libssh2_session_disconnect(session, "host key not trusted");
        libssh2_session_free(session);
        net_close(sock);
        // lost_connection stays 0 - needs the user to re-decide, not a retry.
        goto done;
      }
    }
  }

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
    else if(req.kind == SFTP_ReqKind_Delete)
    {
      Temp req_scratch = scratch_begin(0, 0);
      String8 path = str8(req.path, Min(req.path_size, sizeof(req.path)));
      char *path_cstr = str8_to_cstring(req_scratch.arena, path);
      B32 disc = sftp_do_delete(ring, session, sftp, path_cstr);
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
