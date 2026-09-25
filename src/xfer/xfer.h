// The transfer engine (XFER_ namespace): a non-blocking operations queue.
// Dragging N files just pushes N Queued ops into a fixed array - fast,
// no network activity, UI stays responsive. xfer_dispatch (called once
// per frame) promotes Queued ops to InProgress up to the configured
// concurrency caps, launching a thread per promoted op. Each transfer
// thread opens its OWN independent session (own socket, own SSH
// handshake/auth) rather than sharing/multiplexing the control
// connection's session - simpler, no shared-session locking to get right,
// and what FileZilla itself does. Deliberately duplicates a small connect/
// handshake/auth sequence from sftp_client.c rather than forcing a shared
// abstraction across two different message-reporting protocols.
//
// Mid-transfer cancel IS supported (xfer_cancel, below): a Queued op is
// just removed outright, an InProgress one is asked to stop via its
// cancel_requested flag and reports itself Failed (with a "cancelled by
// user" error_text) once its thread notices and unwinds - same shutdown
// path as any other transfer failure, no separate Cancelled status needed.
// A Failed op (cancelled or genuinely errored) can be continued from where
// it left off via xfer_resume, below - retry-on-failure (automatic resume
// without the user asking) is still out of scope.

#ifndef XFER_H
#define XFER_H

#define XFER_MAX_OPS 256
// libssh2_sftp_read/write pipeline internally - they queue as many
// MAX_SFTP_READ_SIZE/MAX_SFTP_OUTGOING_SIZE (~30KB) protocol packets as fit
// in the buffer passed to a single call, firing them all before waiting on
// any reply, up to ~4x this size (see sftp_read's max_read_ahead) or the SSH
// channel window, whichever is smaller. A small chunk buffer means a shallow
// pipeline - throughput caps at roughly chunk_size / round_trip_time
// regardless of bandwidth. 256KB keeps enough packets in flight (~8-9) to
// saturate real links instead of stop-and-waiting on each one.
#define XFER_CHUNK_SIZE KB(256)
#define XFER_PROGRESS_REPORT_BYTES KB(64) // don't flood the ring - report every ~64KB moved, not every chunk
#define XFER_PROGRESS_REPORT_US 250000    // ...or every 250ms, whichever comes first - keeps slow transfers'
                                           // speed figures fresh even when they're nowhere near 64KB yet
#define XFER_ERROR_MAX 256
#define XFER_SPEED_SAMPLES 8 // a short window (~1-2s at the reporting cadence above) - deliberately tight,
                              // not a long rolling average like FileZilla's (the user specifically wants
                              // real oscillations to show up promptly, not get smoothed into sluggishness)

typedef enum XFER_Kind
{
  XFER_Kind_Download, // remote -> local
  XFER_Kind_Upload,   // local -> remote
}
XFER_Kind;

typedef enum XFER_Status
{
  XFER_Status_Queued,
  XFER_Status_InProgress,
  XFER_Status_Done,
  XFER_Status_Failed,
}
XFER_Status;

typedef struct XFER_Op XFER_Op;
struct XFER_Op
{
  // Stable identity, assigned once at enqueue time and never reused -
  // xfer_cancel_queued/xfer_cancel_by_id shift later ops down by one
  // slot when a Queued op is removed, so a raw array index captured
  // earlier (e.g. by a context menu opened on one frame and resolved on
  // a later one) can silently end up pointing at a different op if
  // anything ahead of it was removed in between. Anything that needs to
  // remember "this op" across more than one frame should hold this, not
  // an index into ops[].
  U64 id;

  XFER_Kind kind;
  String8 local_path;
  String8 remote_path;
  String8 display_name;

  // Credentials copied at op-creation time (drag-drop time), not read from
  // the live connection at dispatch time - so a later reconnect or switch
  // to a different saved profile can't corrupt an already-queued or
  // in-flight transfer.
  String8 host;
  U16 port;
  String8 user;
  String8 pass;

  // The control connection's already-verified host key hash for this
  // (host, port), copied at the same op-creation time as the credentials
  // above. This transfer's own independent session (see the big comment
  // at the top of this file) verifies its handshake's host key against
  // this and fails closed on any mismatch or if it's somehow unset - it
  // never prompts itself (see xfer_thread_entry): by the time a transfer
  // can be enqueued at all, the control connection has already gone
  // through sftp_client.c's TOFU/mismatch prompt, so anything other than
  // an exact match here means a MITM sitting in front of *this specific*
  // connection attempt, not a legitimate first-time-ever endpoint.
  U8 host_key_hash[SFTP_HOST_KEY_HASH_SIZE];

  U64 size_total;
  U64 bytes_done;
  XFER_Status status;
  String8 error_text;

  // Speed: a short rolling window of recent (time,bytes) samples - see
  // XFER_SPEED_SAMPLES above for why it's short. Recomputed from the
  // oldest-to-newest sample in this window every time a new one arrives
  // (xfer__record_speed_sample, in xfer.c). 0 until at least 2 samples
  // exist.
  U64 speed_sample_time_us[XFER_SPEED_SAMPLES];
  U64 speed_sample_bytes[XFER_SPEED_SAMPLES];
  U32 speed_sample_count;
  F64 current_bps;

  // Only ever mutated by the thread that produced it, read via the ring by
  // the main thread in xfer_drain_progress - never written directly to
  // `bytes_done`/`status`/etc. from the transfer thread itself (matches
  // this codebase's "GuardedRing, not shared mutable state" rule).
  GuardedRing progress_ring;
  Thread thread;
  B32 thread_joined;

  // The one deliberate exception to "only the producing thread writes its
  // own fields": the main thread sets this (via xfer_cancel, using
  // ins_atomic_u64_eval_assign) to ask an InProgress transfer to stop; the
  // transfer thread polls it (ins_atomic_u64_eval) once per chunk in its
  // read/write loop. A GuardedRing is the wrong tool for a single one-shot
  // flag nobody needs to block waiting on - a plain word behind the base
  // layer's ins_atomic_ helpers (base_core.h) is simplest and correct here.
  U64 cancel_requested;

  // Set by xfer_resume (main thread) strictly before this op is next
  // dispatched, read once by xfer_thread_entry at open time - same
  // set-before-launch/read-only-during-the-thread's-life contract every
  // other field above (local_path, host, ...) already relies on, so unlike
  // cancel_requested this is a plain B32, no atomic needed.
  B32 resume_requested;

  // Set at enqueue time for ops that came from a folder drag-drop (see
  // XFER_Scan below and main.c's folder drag handling), where some part of
  // the destination's directory chain may genuinely be new. Read once by
  // xfer_thread_entry, which then best-effort creates the destination's
  // parent directory (local: fs_dir_create; remote: mkdir every ancestor
  // path component) before opening the destination for write. Left 0 for
  // an ordinary single-file drag, which never needs this (its destination
  // directory already exists) and shouldn't pay for it.
  B32 ensure_parent_dir;
};

typedef struct XFER_Queue XFER_Queue;
struct XFER_Queue
{
  Arena *arena; // never cleared - ops persist for the app's life (Queue/Failed/Completed history)
  XFER_Op ops[XFER_MAX_OPS];
  U64 op_count;
  U64 next_id;    // monotonic - the next XFER_Op::id to hand out (see its own comment)
  U64 drop_count; // incremented whenever xfer_enqueue silently drops an op because op_count == XFER_MAX_OPS -
                   // the caller (main.c) surfaces this as a visible warning rather than leaving it silent.
};

internal void xfer_queue_init(XFER_Queue *q, Arena *arena);

// Pushes one Queued op (arena-copies all the String8 fields so the caller's
// originals - e.g. a frame_arena path string - don't need to outlive this
// call). No-op (silently drops) if the queue is already at XFER_MAX_OPS.
// `ensure_parent_dir` should be 1 for ops that came from a folder drag
// (see XFER_Op::ensure_parent_dir above), 0 for an ordinary single-file
// drag.
internal void xfer_enqueue(XFER_Queue *q, XFER_Kind kind, String8 local_path, String8 remote_path,
                            String8 display_name, String8 host, U16 port, String8 user, String8 pass,
                            U8 host_key_hash[SFTP_HOST_KEY_HASH_SIZE], B32 ensure_parent_dir);

// Removes a Queued (not yet started) op by index, shifting later ops down.
// No-op if `index` isn't currently Queued (e.g. it started running between
// the caller deciding to cancel and this call arriving - use xfer_cancel
// instead if the op might be InProgress by then).
internal void xfer_cancel_queued(XFER_Queue *q, U64 index);

// Cancels op `index` regardless of its current status: a Queued op is
// removed outright (same as xfer_cancel_queued); an InProgress op is
// signaled via cancel_requested and left running - its thread notices,
// unwinds, and reports Failed ("cancelled by user") through the normal
// xfer_drain_progress path, same as any other transfer failure. No-op for
// an op that's already Done/Failed, or for an out-of-range index.
internal void xfer_cancel(XFER_Queue *q, U64 index);

// Same as xfer_cancel, but by XFER_Op::id rather than array index - use
// this instead of xfer_cancel whenever the caller captured "which op"
// on an earlier frame than the one it acts on (e.g. a context menu),
// since ops[] indices can shift out from under a held index but ids
// never do. No-op if no op with this id currently exists (already
// removed/completed).
internal void xfer_cancel_by_id(XFER_Queue *q, U64 id);

// Re-queues a Failed op to continue from wherever it actually left off,
// rather than restarting from byte 0. No-op unless op->status is Failed.
// The real resume offset is determined by the transfer thread itself from
// the destination's actual current size (see xfer_thread_entry) - not from
// this op's last-known bytes_done - so this is safe to call even if the
// previous attempt's last progress report undercounted what actually landed.
internal void xfer_resume(XFER_Queue *q, U64 index);

// Call once per frame: promotes Queued ops to InProgress (launching their
// thread) up to `max_downloads`/`max_uploads` concurrently-InProgress ops
// of each kind.
internal void xfer_dispatch(XFER_Queue *q, U32 max_downloads, U32 max_uploads);

// Call once per frame: drains every InProgress op's progress ring
// (non-blocking), updating bytes_done/status, joining+finalizing on
// Done/Failed.
internal void xfer_drain_progress(XFER_Queue *q);

// Formats a bytes/sec figure as "1.2 MB/s" / "340 KB/s" / "980 B/s".
internal String8 xfer_format_speed(Arena *arena, F64 bytes_per_sec);

// Formats bytes_remaining/bytes_per_sec as "12s" / "3m 05s" / "1h 12m" -
// "--" if bytes_per_sec isn't established yet (see current_bps above).
internal String8 xfer_format_eta(Arena *arena, U64 bytes_remaining, F64 bytes_per_sec);

// --- folder download: recursive remote scan --------------------------------
//
// A folder drag from the remote pane needs to know what files exist under
// the dragged directory before any XFER_Op can be enqueued for them - and
// finding that out means walking the remote tree over the network. That
// walk can't go through the persistent control connection's ListDir
// request/response ring (SFTP_Req/SFTP_CtrlMsg in sftp_client.h): that
// ring has no request-id, so a multi-request recursive walk's Entry/Done
// messages would be indistinguishable from the visible remote pane's own
// listing traffic (including its periodic auto-refresh) once interleaved
// on the same ring. So instead this is one more one-shot thread with its
// own independent SFTP session, exactly like an ordinary transfer thread
// (see the big comment at the top of this file) - it just walks instead of
// moving bytes, and reports each file it finds back over its own ring
// instead of a byte offset. Once a file is reported, it's turned into a
// perfectly ordinary xfer_enqueue call by the main thread - from that
// point on it's indistinguishable from a plain single-file drag.
//
// A folder's local target directory (the top-level one the user actually
// dragged) is created by the caller, synchronously, before starting the
// scan - see main.c's folder drag handling. Every local subdirectory
// below that is created lazily by each individual downloaded file's own
// ensure_parent_dir (fs_dir_create is mkdir -p, so this needs no
// coordination with the scan at all).
#define XFER_MAX_SCANS 8
#define XFER_SCAN_PATH_MAX 512

typedef enum XFER_ScanMsgKind
{
  XFER_ScanMsgKind_File,   // one file found - rel_path (forward-slash, relative to remote_root) + size_bytes
  XFER_ScanMsgKind_Done,   // scan complete (thread about to exit)
  XFER_ScanMsgKind_Failed, // scan aborted - rel_path holds an error string instead of a path
}
XFER_ScanMsgKind;

typedef struct XFER_ScanMsg XFER_ScanMsg;
struct XFER_ScanMsg
{
  XFER_ScanMsgKind kind;
  U64 size_bytes;
  U64 rel_path_size;
  U8  rel_path[XFER_SCAN_PATH_MAX];
};

typedef struct XFER_Scan XFER_Scan;
struct XFER_Scan
{
  B32 in_use;
  String8 local_root;  // already exists by the time the scan starts
  String8 remote_root; // the dragged directory's full remote path
  String8 host;
  U16 port;
  String8 user;
  String8 pass;
  U8 host_key_hash[SFTP_HOST_KEY_HASH_SIZE]; // see XFER_Op's own field of the same name/purpose above
  GuardedRing ring;
  Thread thread;
};

// Finds a free slot in `scans` (array of `scan_count`) and launches a scan
// of `remote_root` into `local_root` on its own thread. Silently drops the
// request if every slot is already in use (XFER_MAX_SCANS concurrent
// folder-drag downloads is far beyond normal use) - same "just drop it,
// don't block or queue" precedent as xfer_enqueue at XFER_MAX_OPS.
internal void xfer_scan_start(XFER_Scan *scans, U64 scan_count, Arena *arena,
                                String8 local_root, String8 remote_root,
                                String8 host, U16 port, String8 user, String8 pass,
                                U8 host_key_hash[SFTP_HOST_KEY_HASH_SIZE]);

// Call once per frame: drains every active scan's ring (non-blocking),
// turning each discovered file straight into an xfer_enqueue call, and
// frees the slot (joining its thread) once the scan reports Done/Failed -
// appending one human-readable line to `log_lines` (allocated from
// `log_arena`) either way, the same text-line convention the Log tab
// already uses for the control connection's own Log/Error messages.
internal void xfer_scan_poll_all(XFER_Scan *scans, U64 scan_count, XFER_Queue *xfer_queue,
                                   Arena *log_arena, String8List *log_lines);

#endif // XFER_H
