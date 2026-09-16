// The control-connection thread: owns the SSH/SFTP session for one server
// (connect, password auth, directory listing so far), reporting results
// back to whatever's listening (console today, GUI later) as fixed-size
// messages over a GuardedRing rather than shared state.
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

typedef enum SFTP_CtrlMsgKind
{
  SFTP_CtrlMsgKind_Log,   // one operation/command being performed - text
  SFTP_CtrlMsgKind_Entry, // one directory entry
  SFTP_CtrlMsgKind_Error, // connect/auth/protocol failure - text
  SFTP_CtrlMsgKind_Done,  // no more messages coming; thread is exiting
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
};

typedef struct SFTP_ConnectParams SFTP_ConnectParams;
struct SFTP_ConnectParams
{
  String8 host;
  U16 port;
  String8 username;
  String8 password;
  String8 remote_dir;
  GuardedRing *out_ring;
};

// Entry point for thread_launch. Connects, authenticates, lists
// remote_dir, and disconnects - sending SFTP_CtrlMsg messages the whole
// way, always ending with exactly one SFTP_CtrlMsgKind_Done.
internal void sftp_control_thread_entry(void *ptr);

#endif // SFTP_CLIENT_H
