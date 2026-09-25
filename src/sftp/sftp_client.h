// The control-connection thread: owns the SSH/SFTP session for one server
// (connect, password auth, and now repeated directory listings), reporting
// results back to whatever's listening (console/GUI) as fixed-size
// messages over a GuardedRing rather than shared state, and accepting
// on-demand "list this other directory" requests over a second, inbound
// GuardedRing - so the thread stays alive and connected for the whole
// session instead of doing one listing and exiting.
//
// This includes a running log of every operation the thread performs
// ("connecting", "authenticating", "opening directory", ...) - SFTP has no
// textual command stream to mirror the way plain FTP does, so this log of
// logical steps is the equivalent. It's a message kind in the same stream
// as directory entries specifically so a future GUI log pane and today's
// console line-printing loop are the same consumer code, just rendering
// differently.

#ifndef SFTP_CLIENT_H
#define SFTP_CLIENT_H

#define SFTP_NAME_MAX 512
#define SFTP_TEXT_MAX 256
#define SFTP_HOST_KEY_HASH_SIZE 32 // SHA256 - see sftp_verify_or_prompt_host_key

typedef enum SFTP_CtrlMsgKind
{
  SFTP_CtrlMsgKind_Log,       // one operation/command being performed - text
  SFTP_CtrlMsgKind_Entry,     // one directory entry
  SFTP_CtrlMsgKind_Error,     // a normal SFTP-protocol-level failure (bad path, permissions, ...) - text. Session is still fine.
  SFTP_CtrlMsgKind_Done,      // the current listing's entries are all sent (thread keeps running)
  SFTP_CtrlMsgKind_ThreadExit,   // thread is returning because it was asked to (Quit) or hit an
                                 // unretryable failure (bad credentials) - join it, do NOT reconnect.
  SFTP_CtrlMsgKind_Disconnected, // thread is returning because the connection itself died (any
                                 // transport-level failure, at any point) - join it, then reconnect.
  SFTP_CtrlMsgKind_HostKeyVerify, // the server's host key needs the user's OK before auth proceeds -
                                  // host_key_hash/host_key_is_change carry the details (see below).
                                  // The thread blocks (no timeout - this is a human decision, not
                                  // network I/O) until it sees HostKeyAccept/HostKeyReject/Quit on
                                  // in_ring.
}
SFTP_CtrlMsgKind;

typedef struct SFTP_CtrlMsg SFTP_CtrlMsg;
struct SFTP_CtrlMsg
{
  SFTP_CtrlMsgKind kind;
  U64 timestamp_us; // now_time_us() when this message was produced (Log/Error)

  // SFTP_CtrlMsgKind_Entry
  B32 is_dir;
  U64 size_bytes;
  U64 name_size;
  U8  name[SFTP_NAME_MAX];

  // SFTP_CtrlMsgKind_Log / SFTP_CtrlMsgKind_Error
  U64 text_size;
  U8  text[SFTP_TEXT_MAX];

  // SFTP_CtrlMsgKind_HostKeyVerify: the SHA256 host key hash the server
  // just presented, and whether this replaces a previously-pinned
  // *different* hash (1 - the server's key changed, or this could be an
  // impersonation attempt) vs. this endpoint has never been seen before
  // (0 - ordinary first connection, trust-on-first-use).
  U8  host_key_hash[SFTP_HOST_KEY_HASH_SIZE];
  B32 host_key_is_change;
};

typedef enum SFTP_ReqKind
{
  SFTP_ReqKind_ListDir, // path is the new directory to list
  SFTP_ReqKind_Delete,  // path is the file to delete - directories aren't supported yet
  SFTP_ReqKind_Quit,    // finish current work, disconnect, send ThreadExit, return
  SFTP_ReqKind_HostKeyAccept, // user confirmed the pending HostKeyVerify prompt - proceed to auth
  SFTP_ReqKind_HostKeyReject, // user declined it - disconnect without authenticating
}
SFTP_ReqKind;

typedef struct SFTP_Req SFTP_Req;
struct SFTP_Req
{
  SFTP_ReqKind kind;
  U64 path_size;
  U8  path[SFTP_NAME_MAX];
};

typedef struct SFTP_ConnectParams SFTP_ConnectParams;
struct SFTP_ConnectParams
{
  String8 host;
  U16 port;
  String8 username;
  String8 password;
  String8 remote_dir;
  GuardedRing *out_ring; // control -> caller: SFTP_CtrlMsg
  GuardedRing *in_ring;  // caller -> control: SFTP_Req

  // The caller's current known-hosts lookup result for (host, port) -
  // see known_hosts_store.h. If has_pinned_host_key_hash is 0, this is
  // the first time this endpoint has ever been connected to and the
  // control thread always prompts (SFTP_CtrlMsgKind_HostKeyVerify)
  // before authenticating. If it's 1, the thread compares silently and
  // only prompts on a *mismatch* (host_key_is_change = 1).
  U8  pinned_host_key_hash[SFTP_HOST_KEY_HASH_SIZE];
  B32 has_pinned_host_key_hash;
};

// Entry point for thread_launch. Connects, authenticates, lists
// remote_dir, then services SFTP_Req requests from in_ring (each producing
// another listing) until asked to quit or the connection dies. Ends with
// exactly one of SFTP_CtrlMsgKind_ThreadExit (deliberate Quit, or bad
// credentials - caller should not reconnect) or
// SFTP_CtrlMsgKind_Disconnected (any transport-level failure, whether
// during initial connect or later - caller should reconnect, using the
// same credentials and whatever remote directory it was last showing).
// Every listing (the initial one and each subsequent one) that doesn't
// itself trip a disconnect ends with exactly one SFTP_CtrlMsgKind_Done.
//
// Known limitation, accepted: requests are only checked for *between*
// listings, not mid-listing - fine given how fast a listing is, so no
// cancellation/interruption machinery exists.
internal void sftp_control_thread_entry(void *ptr);

// Forward-slash SFTP path helpers (protocol-layer paths, not Windows
// filesystem paths - see fs_local.h for those). "." is the connection's
// starting directory and has no parent.
internal String8 sftp_path_join(Arena *arena, String8 dir, String8 name);
internal String8 sftp_path_parent(Arena *arena, String8 path);

#endif // SFTP_CLIENT_H
