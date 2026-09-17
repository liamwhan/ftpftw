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
// Resume, retry-on-failure, and mid-transfer cancel are explicitly out of
// scope for this pass - a Queued (not yet started) op can be removed, but
// an InProgress one runs to completion or failure.

#ifndef XFER_H
#define XFER_H

#define XFER_MAX_OPS 256
#define XFER_CHUNK_SIZE KB(32)
#define XFER_PROGRESS_REPORT_BYTES KB(64) // don't flood the ring - report every ~64KB moved, not every chunk
#define XFER_ERROR_MAX 256

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

  U64 size_total;
  U64 bytes_done;
  XFER_Status status;
  String8 error_text;

  // Only ever mutated by the thread that produced it, read via the ring by
  // the main thread in xfer_drain_progress - never written directly to
  // `bytes_done`/`status`/etc. from the transfer thread itself (matches
  // this codebase's "GuardedRing, not shared mutable state" rule).
  GuardedRing progress_ring;
  Thread thread;
  B32 thread_joined;
};

typedef struct XFER_Queue XFER_Queue;
struct XFER_Queue
{
  Arena *arena; // never cleared - ops persist for the app's life (Queue/Failed/Completed history)
  XFER_Op ops[XFER_MAX_OPS];
  U64 op_count;
};

internal void xfer_queue_init(XFER_Queue *q, Arena *arena);

// Pushes one Queued op (arena-copies all the String8 fields so the caller's
// originals - e.g. a frame_arena path string - don't need to outlive this
// call). No-op (silently drops) if the queue is already at XFER_MAX_OPS.
internal void xfer_enqueue(XFER_Queue *q, XFER_Kind kind, String8 local_path, String8 remote_path,
                            String8 display_name, String8 host, U16 port, String8 user, String8 pass);

// Removes a Queued (not yet started) op by index, shifting later ops down.
// No-op if `index` isn't currently Queued (e.g. it started running between
// the caller deciding to cancel and this call - cancelling an in-progress
// transfer isn't supported this pass).
internal void xfer_cancel_queued(XFER_Queue *q, U64 index);

// Call once per frame: promotes Queued ops to InProgress (launching their
// thread) up to `max_downloads`/`max_uploads` concurrently-InProgress ops
// of each kind.
internal void xfer_dispatch(XFER_Queue *q, U32 max_downloads, U32 max_uploads);

// Call once per frame: drains every InProgress op's progress ring
// (non-blocking), updating bytes_done/status, joining+finalizing on
// Done/Failed.
internal void xfer_drain_progress(XFER_Queue *q);

#endif // XFER_H
