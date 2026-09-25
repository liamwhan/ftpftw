// UI structure: toolbar (now with a Connections button), local/remote
// directory-tree panes (right-click a row to delete - see the two
// UI_ContextMenuState/UI_ConfirmState pairs below), tabbed bottom pane
// (Log/Queue/Failed/Completed). sftp_client.h/.c gained one more request
// kind (Delete, alongside ListDir/Quit) but no architecture change - same
// GuardedRing protocol either way.

#include "version.h"

#include "base/base_inc.h"
#include "base/base_inc.c"

#include "wm/wm_core.h"
#include "wm/win32/wm_core_win32.c"

#include "render/r_core.h"
#include "render/d3d11/r_d3d11.c"

// fp_dwrite is implemented in fp_dwrite.cpp, compiled and linked
// separately (see build.bat) - <dwrite.h> isn't C-includable, see
// fp_dwrite.h for why. Only its plain-C-callable header comes in here.
#include "font/fp_dwrite.h"
#include "font/fnt_cache.h"
#include "font/fnt_cache.c"

#include "draw/dr_core.h"
#include "draw/dr_core.c"

#include "fs/fs_local.h"
#include "fs/fs_local.c"

#include "ui/ui_icons.h"
#include "ui/ui_icons.c"
#include "ui/ui_core.h"
#include "ui/ui_core.c"

#include "store/conn_store.h"
#include "store/conn_store.c"
#include "store/xfer_settings.h"
#include "store/xfer_settings.c"
#include "store/local_dir_store.h"
#include "store/local_dir_store.c"
#include "store/known_hosts_store.h"
#include "store/known_hosts_store.c"

#include "ui/ui_conn_modal.h"
#include "ui/ui_conn_modal.c"
#include "ui/ui_settings_modal.h"
#include "ui/ui_settings_modal.c"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "sftp/sftp_client.h"
#include "sftp/sftp_client.c"

#include "xfer/xfer.h"
#include "xfer/xfer.c"

#define TOOLBAR_H     40.0f
#define BOTTOM_PANE_H 200.0f
#define TAB_STRIP_H   28.0f
#define ROW_H         22.0f
#define MAX_REMOTE_ENTRIES 1024

// Resizable local/remote and top/bottom dividers - see the "resizable
// divider handling" block in main()'s loop.
#define MIN_PANE_W      150.0f // minimum local/remote pane width, each side
#define MIN_TOP_H       120.0f // minimum combined local/remote pane height
#define MIN_BOTTOM_H    80.0f  // minimum bottom (Log/Queue/Failed/Completed) pane height
#define DIVIDER_HIT_PAD 5.0f   // how many px on either side of a divider line count as "on it"

// Reconnect backoff: doubles on every consecutive failed attempt rather
// than retrying a genuinely-down server every 2s forever (which just
// keeps hammering it and burning CPU/network for no benefit) - reset to
// the minimum by connect_to and by any listing that actually completes.
#define RECONNECT_BACKOFF_MIN_S 2
#define RECONNECT_BACKOFF_MAX_S 60

// Caps how many lines the Log tab keeps - unbounded growth here was the
// only thing in `arena` (the app-lifetime arena) that scales with wall-
// clock session length rather than a fixed count of profiles/ops, so a
// long-running session on a flaky connection (reconnect messages, one
// line each) would otherwise grow memory without bound.
#define LOG_MAX_LINES 4000

internal String8
env_str8(Arena *arena, char *name)
{
  char *value = getenv(name);
  return value ? str8_cstring_copy(arena, value) : (String8){0};
}

// Log tab text: Log/Error messages only - Entry messages become remote
// tree rows (see RemoteConn below) instead of log-style text lines.
internal String8
line_from_ctrl_msg(Arena *arena, SFTP_CtrlMsg *msg, U64 log_start_us)
{
  String8 result = {0};
  F64 elapsed_s = (F64)(msg->timestamp_us - log_start_us) / 1000000.0;
  switch(msg->kind)
  {
    default: break;
    case SFTP_CtrlMsgKind_Log:
    {
      result = str8f(arena, "[%7.3f] %.*s", elapsed_s, (int)msg->text_size, (char *)msg->text);
    }break;
    case SFTP_CtrlMsgKind_Error:
    {
      result = str8f(arena, "[%7.3f] error: %.*s", elapsed_s, (int)msg->text_size, (char *)msg->text);
    }break;
  }
  return result;
}

// Allocates a fresh session arena and launches a new control-connection
// thread against it - used both by connect_to (below) and the reconnect
// timer. The GuardedRings and SFTP_ConnectParams must stay valid for the
// whole life of the thread (it reads params->out_ring/in_ring throughout,
// not just at startup), so they're pushed from session_arena rather than
// being stack locals here - session_arena itself is only released after
// the thread has fully exited (ThreadExit/Disconnected).
internal Thread
start_sftp_session(Arena *session_arena, String8 host, U16 port, String8 user, String8 pass,
                    String8 remote_dir, B32 has_pinned_host_key_hash, U8 pinned_host_key_hash[SFTP_HOST_KEY_HASH_SIZE],
                    GuardedRing **out_ring, GuardedRing **out_in_ring)
{
  GuardedRing *ring = push_array(session_arena, GuardedRing, 1);
  *ring = guarded_ring_alloc(session_arena, KB(64));
  GuardedRing *in_ring = push_array(session_arena, GuardedRing, 1);
  *in_ring = guarded_ring_alloc(session_arena, KB(4));

  SFTP_ConnectParams *params = push_array(session_arena, SFTP_ConnectParams, 1);
  params->host = host;
  params->port = port;
  params->username = user;
  params->password = pass;
  params->remote_dir = remote_dir;
  params->out_ring = ring;
  params->in_ring = in_ring;
  params->has_pinned_host_key_hash = has_pinned_host_key_hash;
  if(has_pinned_host_key_hash)
  {
    MemoryCopy(params->pinned_host_key_hash, pinned_host_key_hash, SFTP_HOST_KEY_HASH_SIZE);
  }

  *out_ring = ring;
  *out_in_ring = in_ring;
  return thread_launch(sftp_control_thread_entry, params);
}

// Local->remote folder drag: the whole local tree is already sitting on
// disk, so unlike the remote->local direction (see XFER_Scan in xfer.h)
// this needs no async scan - just walk it synchronously (cheap: local
// FindFirstFile calls, no network) and enqueue one ensure_parent_dir
// upload per file found. A directory with no files anywhere below it
// never gets created remotely (nothing to hang the mkdir off of - see
// XFER_Op::ensure_parent_dir) - an accepted gap, not a "recreate empty
// directories too" feature.
internal void
local_enqueue_folder_upload(Arena *scratch_arena, XFER_Queue *xfer_queue,
                             String8 local_dir, String8 remote_dir,
                             String8 host, U16 port, String8 user, String8 pass,
                             U8 host_key_hash[SFTP_HOST_KEY_HASH_SIZE])
{
  FS_EntryArray entries = fs_list_dir(scratch_arena, local_dir);
  for(U64 i = 0; i < entries.count; i += 1)
  {
    FS_Entry *e = &entries.v[i];
    if(str8_match(e->name, str8_lit(".."))) { continue; }
    String8 local_full = fs_path_join(scratch_arena, local_dir, e->name);
    String8 remote_full = sftp_path_join(scratch_arena, remote_dir, e->name);
    if(e->is_dir)
    {
      local_enqueue_folder_upload(scratch_arena, xfer_queue, local_full, remote_full, host, port, user, pass, host_key_hash);
    }
    else
    {
      xfer_enqueue(xfer_queue, XFER_Kind_Upload, local_full, remote_full, e->name, host, port, user, pass, host_key_hash, 1);
    }
  }
}

// Everything about "the current remote connection and what the remote
// pane is showing" bundled together - these were 15 separate locals in
// main() before the Connections modal made "connect to a different target
// at runtime" a real operation, not just a startup-time constant; grouping
// them is what actually keeps connect_to/teardown_live_session's
// signatures flat rather than each taking a dozen individual pointers.
typedef struct RemoteConn RemoteConn;
struct RemoteConn
{
  // session/thread lifetime
  Arena *session_arena; // torn down and recreated per connection attempt
  GuardedRing *ring;
  GuardedRing *in_ring;
  Thread control;
  B32 control_joined;
  B32 connecting;         // true once any connection attempt has ever been made - never goes back to false
  B32 active;             // true whenever there's a live-or-should-reconnect session; false once the user
                           // manually disconnects - lets Disconnect actually stop the auto-reconnect timer
                           // without also having to unwind `connecting`'s (deliberately) one-way meaning
  B32 awaiting_reconnect;
  U64 reconnect_at_us;
  U32 reconnect_backoff_s; // doubles on every consecutive failed (re)connect, capped at
                            // RECONNECT_BACKOFF_MAX_S - reset to RECONNECT_BACKOFF_MIN_S by connect_to
                            // and by any listing that actually completes, so a flaky-but-sometimes-up
                            // link doesn't stay penalized once it's genuinely back.
  U64 log_start_us;

  // credentials - copied into creds_arena, which outlives session_arena
  // (a reconnect to the same target reuses these; connect_to replaces them)
  Arena *creds_arena;
  String8 host, user, pass;
  U16 port;

  // The pinned/just-verified SSH host key fingerprint for (host, port) -
  // see known_hosts_store.h and sftp_client.h's SFTP_CtrlMsgKind_HostKeyVerify.
  // Looked up (has_host_key_hash may come back 0 - first-ever connection)
  // at the top of connect_to, and updated once the user accepts a
  // HostKeyVerify prompt. Every transfer/scan thread started against this
  // connection is handed a copy (see the xfer_enqueue/xfer_scan_start call
  // sites below) so it can verify its own independent session's host key
  // without prompting anyone itself.
  B32 has_host_key_hash;
  U8  host_key_hash[SFTP_HOST_KEY_HASH_SIZE];

  // A HostKeyVerify prompt is currently up (host_key_confirm, in main())
  // and waiting on the user - the hash/is_change it's asking about, held
  // here rather than in UI_ConfirmState (which only carries display text)
  // so the confirm/reject handler below knows what it's actually deciding.
  B32 host_key_prompt_pending;
  U8  host_key_prompt_hash[SFTP_HOST_KEY_HASH_SIZE];
  B32 host_key_prompt_is_change;

  // Every pinned host key fingerprint ever accepted, loaded once at
  // startup into the app-lifetime arena (see main()) and updated (plus
  // re-saved to disk) whenever a HostKeyVerify prompt is accepted.
  KH_HostArray kh_hosts;

  // remote pane display state - reset on every connect_to, cleared and
  // repopulated on every navigation/reconnect
  Arena *remote_arena;
  String8 remote_path;
  FS_Entry entries[MAX_REMOTE_ENTRIES];
  B32 selected[MAX_REMOTE_ENTRIES];
  U64 entry_count;
  B32 sorted;
  UI_RowListState row_state;
};

// Signals the live control thread to quit and blocks until it's fully
// exited, then releases its rings and session arena. Used both by
// connect_to (switching to a different target while connected) and final
// shutdown. Blocking here means a brief UI freeze while the old
// connection tears down (session disconnect, socket close) - acceptable
// for a first pass; a fully non-blocking teardown would need its own
// polled state machine, not worth it yet for what's normally sub-second.
//
// Bounded, not indefinite: sftp_client.c now sets a libssh2 session
// timeout (SFTP_SESSION_TIMEOUT_MS) so the control thread can never be
// stuck inside one blocking libssh2 call for longer than that against a
// stalled/black-holed peer - but this wait still needs its own generous-
// but-finite ceiling on top, in case the thread is instead parked in the
// (untimed, waits on a human) host-key-prompt gate with nobody left to
// answer it (e.g. window closing while a HostKeyVerify prompt is up). If
// the thread hasn't acknowledged Quit by TEARDOWN_MAX_WAIT_US, detach and
// move on rather than freeze the whole window waiting for it - the arena/
// rings are deliberately leaked in that case (the thread might still read
// them), a one-time cost paid only in this already-unusual situation.
#define TEARDOWN_MAX_WAIT_US (30 * 1000000)
internal B32
teardown_live_session(RemoteConn *rc)
{
  SFTP_Req quit_req = {0};
  quit_req.kind = SFTP_ReqKind_Quit;
  guarded_ring_write_struct_or_wait(rc->in_ring, &quit_req, max_U64);

  U64 deadline_us = now_time_us() + TEARDOWN_MAX_WAIT_US;
  B32 exited = 0;
  while(!exited)
  {
    SFTP_CtrlMsg msg = {0};
    if(!guarded_ring_read_struct_or_wait(rc->ring, &msg, deadline_us))
    {
      break; // timed out - thread still hasn't acknowledged Quit
    }
    if(msg.kind == SFTP_CtrlMsgKind_ThreadExit || msg.kind == SFTP_CtrlMsgKind_Disconnected)
    {
      exited = 1;
    }
  }
  if(exited)
  {
    thread_join(rc->control, max_U64);
    guarded_ring_release(rc->ring);
    guarded_ring_release(rc->in_ring);
    arena_release(rc->session_arena);
  }
  else
  {
    thread_detach(rc->control);
  }
  return exited;
}

// Connects (or reconnects to a *different* target) - tears down whatever's
// currently live first, always starts fresh at the root directory (unlike
// the automatic reconnect-on-disconnect timer, which resumes at whatever
// remote_path was last showing for the *same* target). Called both at
// startup (env-var fallback) and from the Connections modal's Connect
// button.
internal void
connect_to(RemoteConn *rc, String8 host, U16 port, String8 user, String8 pass)
{
  if(!rc->control_joined)
  {
    teardown_live_session(rc);
  }
  rc->awaiting_reconnect = 0;

  arena_clear(rc->creds_arena);
  rc->host = str8_copy(rc->creds_arena, host);
  rc->user = str8_copy(rc->creds_arena, user);
  rc->pass = str8_copy(rc->creds_arena, pass);
  rc->port = port;

  arena_clear(rc->remote_arena);
  rc->remote_path = str8_copy(rc->remote_arena, str8_lit("."));
  rc->entry_count = 0;
  rc->sorted = 0;
  MemoryZeroArray(rc->selected);
  rc->row_state.last_click_index = -1;
  rc->row_state.shift_anchor_index = -1;
  rc->row_state.scroll_y = 0.0f;

  rc->log_start_us = now_time_us();
  rc->reconnect_backoff_s = RECONNECT_BACKOFF_MIN_S;
  rc->host_key_prompt_pending = 0;
  rc->has_host_key_hash = kh_store_find(&rc->kh_hosts, rc->host, rc->port, rc->host_key_hash);
  rc->session_arena = arena_alloc();
  rc->control = start_sftp_session(rc->session_arena, rc->host, rc->port, rc->user, rc->pass, rc->remote_path,
                                    rc->has_host_key_hash, rc->host_key_hash, &rc->ring, &rc->in_ring);
  rc->control_joined = 0;
  rc->connecting = 1;
  rc->active = 1;
}

// Re-lists the CURRENTLY-DISPLAYED local directory in place (path
// unchanged) - called when a download starts or ends in exactly this
// directory, so the pane picks up the new/resized file without the user
// having to navigate away and back to force a re-list. Mirrors the local
// double-click-navigation block in main(), minus the actual path change -
// same clear-and-recreate of local_arena, copying local_path out to
// scratch_arena first since that's the very string this is about to clear
// out from under.
internal void
local_refresh_listing(Arena *local_arena, Arena *scratch_arena, String8 *local_path,
                        FS_EntryArray *local_entries, B32 **local_selected, UI_RowListState *local_row_state)
{
  String8 path_copy = str8_copy(scratch_arena, *local_path);
  arena_clear(local_arena);
  *local_path = str8_copy(local_arena, path_copy);
  *local_entries = fs_list_dir(local_arena, *local_path);
  *local_selected = push_array(local_arena, B32, local_entries->count);
  local_row_state->last_click_index = -1;
  local_row_state->shift_anchor_index = -1;
}

// Re-lists the CURRENTLY-DISPLAYED remote directory in place (path
// unchanged) - called when an upload starts or ends in exactly this
// directory. Same idea as local_refresh_listing but async: this only fires
// the ListDir request and returns immediately - the actual FS_Entry rows
// arrive later through the normal SFTP_CtrlMsgKind_Entry drain in main()'s
// loop, exactly like a navigation's listing does today.
internal void
remote_refresh_listing(Arena *scratch_arena, RemoteConn *rc)
{
  String8 path_copy = str8_copy(scratch_arena, rc->remote_path);
  arena_clear(rc->remote_arena);
  rc->remote_path = str8_copy(rc->remote_arena, path_copy);
  rc->entry_count = 0;
  rc->sorted = 0;
  MemoryZeroArray(rc->selected);
  rc->row_state.last_click_index = -1;
  rc->row_state.shift_anchor_index = -1;

  SFTP_Req req = {0};
  req.kind = SFTP_ReqKind_ListDir;
  req.path_size = Min(rc->remote_path.size, sizeof(req.path));
  MemoryCopy(req.path, rc->remote_path.str, req.path_size);
  guarded_ring_write_struct_or_wait(rc->in_ring, &req, max_U64);
}

int
main(void)
{
  os_init();

  TCTX *tctx = tctx_alloc();
  tctx_select(tctx);
  set_thread_name(str8_lit("main"));

  wm_init();
  WM_Window window = wm_window_open(str8_lit("FTP-FTW v" FTW_VERSION), 1100, 720);

  r_init();
  r_window_equip(window);

  fp_init();
  fnt_init();
  ui_icons_init();
  FP_Font font = fp_font_open("Segoe UI");

  Arena *arena = arena_alloc();        // app-lifetime: saved connection profiles, pinned host keys
  Arena *log_arena = arena_alloc();    // app-lifetime, but periodically compacted - see log_compact_if_needed
  Arena *frame_arena = arena_alloc();  // cleared every frame
  Arena *local_arena = arena_alloc();  // local path/listing - cleared on every navigation

  // --- local pane state: starts at the last-visited directory (see
  // local_dir_store.h), falling back to %USERPROFILE% on first run or if
  // that directory no longer exists (moved/deleted since last time) -------
  String8 local_path;
  {
    String8 saved_dir = local_dir_store_load(frame_arena);
    local_path = (saved_dir.size != 0 && fs_dir_exists(saved_dir))
               ? str8_copy(local_arena, saved_dir)
               : str8_copy(local_arena, env_str8(frame_arena, "USERPROFILE"));
  }
  FS_EntryArray local_entries = fs_list_dir(local_arena, local_path);
  UI_RowListState local_row_state = {0};
  local_row_state.last_click_index = -1;
  local_row_state.shift_anchor_index = -1;
  B32 *local_selected = push_array(local_arena, B32, local_entries.count);

  // --- Connections modal ------------------------------------------------------
  UI_ConnModal conn_modal = {0};
  ui_conn_modal_init(&conn_modal, arena);

  // --- Transfer settings modal --------------------------------------------------
  XFER_Settings xfer_settings = xfer_settings_load();
  UI_SettingsModal settings_modal = {0};
  ui_settings_modal_init(&settings_modal);

  // --- remote connection + pane state ------------------------------------------
  // libssh2_init/exit bracket the whole process lifetime exactly once,
  // independent of whether/when/how many times we actually connect
  // (env vars at startup, never, or only later via the modal).
  libssh2_init(0);

  RemoteConn rc = {0};
  rc.control_joined = 1;
  rc.creds_arena = arena_alloc();
  rc.remote_arena = arena_alloc();
  rc.row_state.last_click_index = -1;
  rc.row_state.shift_anchor_index = -1;
  rc.kh_hosts = kh_store_load(arena);

  String8List lines = {0};

#if BUILD_DEBUG
  // Env-var dev auto-connect - debug builds only. getenv() puts the
  // plaintext password into this process's environment block, which any
  // other process running as the same user can read (via
  // CreateToolhelp32Snapshot/ReadProcessMemory or simply
  // GetEnvironmentStrings on itself if launched to inherit it) - fine for
  // a developer's own throwaway test server, but this pattern (env vars
  // in a launcher script/shortcut) should never reach a release build a
  // real user might copy for their real credentials.
  {
    String8 env_host = env_str8(frame_arena, "FTP_HOST");
    String8 env_user = env_str8(frame_arena, "FTP_USER");
    String8 env_pass = env_str8(frame_arena, "FTP_PASS");
    String8 env_port_str = env_str8(frame_arena, "FTP_PORT");
    if(env_host.size != 0 && env_user.size != 0 && env_pass.size != 0)
    {
      U16 env_port = env_port_str.size ? (U16)atoi((char *)env_port_str.str) : 22;
      if(env_port == 0)
      {
        env_port = 22;
      }
      connect_to(&rc, env_host, env_port, env_user, env_pass);
    }
    else
    {
      str8_list_push(log_arena, &lines,
                      str8_lit("not connected - set FTP_HOST/FTP_USER/FTP_PASS(/FTP_PORT) for a dev auto-connect, "
                               "or use the Connections button in the toolbar"));
    }
  }
#else
  str8_list_push(log_arena, &lines, str8_lit("not connected - use the Connections button in the toolbar"));
#endif

  S32 bottom_tab = 0;
  String8 tab_labels[4] = { str8_lit("Log"), str8_lit("Queue"), str8_lit("Failed"), str8_lit("Completed") };

  // Log tab: scroll_from_top in [0, max_scroll], auto-follows the latest
  // line (scroll_from_top snapped to max_scroll every frame) until the
  // user scrolls the wheel over it, same as a terminal/log viewer.
  F32 log_scroll_from_top = 0.0f;
  B32 log_auto_follow = 1;

  // Queue/Failed/Completed tabs: same mouse-wheel-scroll model as the Log
  // tab above (and ui_row_list's own panes), minus auto-follow - one offset
  // per tab since each has its own row count and the user may leave any of
  // them scrolled independently.
  F32 queue_scroll_y = 0.0f;
  F32 failed_scroll_y = 0.0f;
  F32 completed_scroll_y = 0.0f;

  // --- transfer queue -----------------------------------------------------------
  Arena *xfer_arena = arena_alloc(); // never cleared - ops persist for Queue/Failed/Completed history
  XFER_Queue xfer_queue;
  xfer_queue_init(&xfer_queue, xfer_arena);

  // Remote->local folder drags in flight (see XFER_Scan in xfer.h) - a
  // fixed pool, same "just drop it if every slot's in use" precedent as
  // XFER_Queue's own fixed ops[] array.
  XFER_Scan folder_scans[XFER_MAX_SCANS] = {0};

  // Right-click context menu on Queue-tab rows - one shared instance since
  // only one row's menu can be open at a time; state->tag holds the op
  // index it was opened for.
  UI_ContextMenuState xfer_ctx_menu = {0};
  String8 xfer_ctx_menu_labels[1] = { str8_lit("Cancel Transfer") };

  // Right-click context menu on local/remote pane rows - same one-shared-
  // instance-per-kind model as xfer_ctx_menu above. Deleting is destructive
  // (unlike Cancel Transfer, which just stops something in-flight), so
  // choosing the menu's one item opens a confirm dialog rather than acting
  // immediately - see the two UI_ConfirmState instances below.
  UI_ContextMenuState local_ctx_menu = {0};
  UI_ContextMenuState remote_ctx_menu = {0};
  String8 delete_ctx_menu_labels[1] = { str8_lit("Delete") };
  UI_ConfirmState local_delete_confirm = {0};
  UI_ConfirmState remote_delete_confirm = {0};

  // SSH host key confirmation (trust-on-first-use / a changed host key) -
  // opened from the SFTP_CtrlMsgKind_HostKeyVerify branch above, resolved
  // just below the modals (search for host_key_confirm_was_open).
  UI_ConfirmState host_key_confirm = {0};

  // Intra-app drag-and-drop between the local/remote panes (no Win32 OLE
  // drag-drop - just our own mouse-down/move/up state machine). `active`
  // arms on a press over a pane that already has a selection; `dragging`
  // only becomes true past a small movement threshold, so a plain click
  // doesn't misfire as a drag.
  typedef struct DragState DragState;
  struct DragState { B32 active; B32 dragging; B32 from_remote; F32 start_x, start_y; };
  DragState drag = {0};

  // Resizable panes: local/remote split (x-axis) and bottom-pane height
  // (y-axis) - persist across frames, dragged via the thin strips between
  // panes (see the "resizable divider handling" block below). Initial
  // values match the window's starting size (1100x720) and the old
  // BOTTOM_PANE_H constant; clamped every frame regardless of how they got
  // set, so a window resize can't leave either divider off-screen.
  F32 col_split_x = 550.0f;
  F32 bottom_pane_h = BOTTOM_PANE_H;
  B32 col_split_dragging = 0;
  B32 row_split_dragging = 0;

  B32 should_quit = 0;
  while(!should_quit)
  {
    arena_clear(frame_arena);
    WM_EventList events = wm_get_events(frame_arena);
    for(WM_Event *ev = events.first; ev != 0; ev = ev->next)
    {
      switch(ev->kind)
      {
        default: break;
        case WM_EventKind_Close:  { should_quit = 1; }break;
        case WM_EventKind_Resize: { r_window_resize(ev->width, ev->height); }break;
      }
    }
    if(should_quit)
    {
      break;
    }

    ui_begin_frame(window);

    // Drain whatever the control thread has produced since last frame -
    // non-blocking (see GuardedRing docs): Entry messages become remote
    // tree rows, Log/Error become Log-tab text lines.
    if(rc.connecting && !rc.control_joined)
    {
      for(;;)
      {
        SFTP_CtrlMsg msg = {0};
        if(!guarded_ring_read_struct_or_wait(rc.ring, &msg, now_time_us()))
        {
          break;
        }
        if(msg.kind == SFTP_CtrlMsgKind_Entry)
        {
          if(rc.entry_count < ArrayCount(rc.entries))
          {
            FS_Entry *e = &rc.entries[rc.entry_count];
            e->name = str8_copy(rc.remote_arena, str8(msg.name, msg.name_size));
            e->is_dir = msg.is_dir;
            e->size = msg.size_bytes;
            rc.entry_count += 1;
          }
        }
        else if(msg.kind == SFTP_CtrlMsgKind_HostKeyVerify)
        {
          // The control thread is now blocked waiting for a
          // HostKeyAccept/HostKeyReject response on rc.in_ring - see the
          // host_key_confirm handling below, which sends it.
          rc.host_key_prompt_pending = 1;
          MemoryCopy(rc.host_key_prompt_hash, msg.host_key_hash, SFTP_HOST_KEY_HASH_SIZE);
          rc.host_key_prompt_is_change = msg.host_key_is_change;
          String8 fingerprint = sftp_format_host_key_hash(frame_arena, msg.host_key_hash);
          String8 message = msg.host_key_is_change
            ? str8f(frame_arena,
                     "WARNING: the host key for %.*s:%u has CHANGED since you last connected.\n"
                     "New SHA256 fingerprint:\n%.*s\n\n"
                     "This can happen after a legitimate server rebuild, but it is also exactly "
                     "what a network attack looks like. Only Trust this if you are certain why "
                     "it changed.",
                     (int)rc.host.size, (char *)rc.host.str, (U32)rc.port,
                     (int)fingerprint.size, (char *)fingerprint.str)
            : str8f(frame_arena,
                     "The host key for %.*s:%u is not yet known.\n"
                     "SHA256 fingerprint:\n%.*s\n\n"
                     "If you have no way to verify this out of band, only Trust it if this is "
                     "the expected first connection to your own server.",
                     (int)rc.host.size, (char *)rc.host.str, (U32)rc.port,
                     (int)fingerprint.size, (char *)fingerprint.str);
          ui_confirm_open(&host_key_confirm, message);
        }
        else
        {
          String8 line = line_from_ctrl_msg(log_arena, &msg, rc.log_start_us);
          if(line.size != 0)
          {
            str8_list_push(log_arena, &lines, line);
          }
        }
        if(msg.kind == SFTP_CtrlMsgKind_Done)
        {
          if(!rc.sorted)
          {
            qsort(rc.entries, rc.entry_count, sizeof(FS_Entry), fs_entry_compare);
            rc.sorted = 1;
          }
          // A listing actually completed - the connection is genuinely
          // healthy again, not just "the TCP handshake succeeded this
          // time" - so a flaky-but-sometimes-up link doesn't stay
          // penalized by whatever backoff a previous failure ran up to.
          rc.reconnect_backoff_s = RECONNECT_BACKOFF_MIN_S;
        }
        if(msg.kind == SFTP_CtrlMsgKind_ThreadExit)
        {
          // Deliberate Quit (window close, or connect_to switching
          // targets - both drain to ThreadExit/Disconnected themselves
          // via teardown_live_session, so this is the "died on its own"
          // case) or bad credentials - either way, don't reconnect.
          thread_join(rc.control, max_U64);
          rc.control_joined = 1;
          break;
        }
        if(msg.kind == SFTP_CtrlMsgKind_Disconnected)
        {
          thread_join(rc.control, max_U64);
          rc.control_joined = 1;
          guarded_ring_release(rc.ring);
          guarded_ring_release(rc.in_ring);
          arena_release(rc.session_arena);
          rc.session_arena = 0;
          rc.ring = 0;
          rc.in_ring = 0;
          arena_clear(rc.remote_arena);
          rc.entry_count = 0;
          rc.sorted = 0;
          MemoryZeroArray(rc.selected);
          rc.row_state.last_click_index = -1;
          rc.row_state.shift_anchor_index = -1;
          rc.awaiting_reconnect = 1;
          rc.reconnect_at_us = now_time_us() + (U64)rc.reconnect_backoff_s * 1000000;
          str8_list_push(log_arena, &lines, str8f(log_arena, "reconnecting in %us...", rc.reconnect_backoff_s));
          rc.reconnect_backoff_s = Min(rc.reconnect_backoff_s * 2, (U32)RECONNECT_BACKOFF_MAX_S);
          break;
        }
      }
    }

    // A reconnect never fires the instant it's scheduled - a short delay
    // avoids hammering an unreachable server in a tight fail loop. Resumes
    // at the same remote_path (unlike connect_to, which always resets to
    // root) since this is the *same* target coming back, not a switch.
    if(rc.connecting && rc.active && rc.awaiting_reconnect && now_time_us() >= rc.reconnect_at_us)
    {
      rc.log_start_us = now_time_us();
      rc.session_arena = arena_alloc();
      rc.control = start_sftp_session(rc.session_arena, rc.host, rc.port, rc.user, rc.pass, rc.remote_path,
                                       rc.has_host_key_hash, rc.host_key_hash, &rc.ring, &rc.in_ring);
      rc.control_joined = 0;
      rc.awaiting_reconnect = 0;
    }

    // Non-blocking: promotes Queued ops to InProgress up to the configured
    // caps, then drains whatever progress/completion messages any
    // currently-InProgress op's own independent session has produced.
    //
    // Snapshotting status just before this and diffing after lets us
    // detect exactly the two transitions that matter for pane-refreshing
    // (Queued->InProgress = "started", InProgress->Done/Failed = "ended")
    // regardless of *why* an op ended up there (natural completion, a
    // right-click Cancel, or a Resume re-arriving at InProgress) - xfer.c
    // itself never needs to know the UI cares about this at all.
    XFER_Status xfer_prev_status[XFER_MAX_OPS];
    U64 xfer_prev_op_count = xfer_queue.op_count;
    for(U64 i = 0; i < xfer_prev_op_count; i += 1) { xfer_prev_status[i] = xfer_queue.ops[i].status; }

    xfer_dispatch(&xfer_queue, xfer_settings.max_downloads, xfer_settings.max_uploads);
    xfer_drain_progress(&xfer_queue);

    // Non-blocking: drains discovered files from every in-flight folder
    // download scan straight into xfer_queue as ordinary Download ops (see
    // XFER_Scan in xfer.h), logging its outcome once a scan finishes.
    xfer_scan_poll_all(folder_scans, ArrayCount(folder_scans), &xfer_queue, log_arena, &lines);

    // Bound the Log tab's memory rather than letting a long session on a
    // flaky connection grow it forever - see LOG_MAX_LINES and log_arena's
    // own comment above. Compacting to a smaller target (not exactly the
    // cap) means this only runs once every LOG_MAX_LINES/4 new lines, not
    // on every single push once the cap is first reached.
    if(lines.node_count > LOG_MAX_LINES)
    {
      U64 target = LOG_MAX_LINES * 3 / 4;
      U64 skip = lines.node_count - target;
      String8Node *keep_from = lines.first;
      for(U64 i = 0; i < skip && keep_from != 0; i += 1) { keep_from = keep_from->next; }
      Arena *new_log_arena = arena_alloc();
      String8List new_lines = {0};
      for(String8Node *n = keep_from; n != 0; n = n->next)
      {
        str8_list_push(new_log_arena, &new_lines, str8_copy(new_log_arena, n->string));
      }
      arena_release(log_arena);
      log_arena = new_log_arena;
      lines = new_lines;
    }

    // Refresh whichever pane a just-started/just-ended transfer's
    // destination directory is currently showing - a download changes
    // what's on disk locally, an upload changes what's on the server, and
    // either could happen while the corresponding pane is sitting open
    // looking at that exact directory. Only refreshes if it actually
    // matches what's currently displayed (a transfer to/from some other
    // directory the user has since navigated away from doesn't touch
    // either pane) - see fs_path_parent/sftp_path_parent below.
    {
      B32 need_local_refresh = 0, need_remote_refresh = 0;
      for(U64 i = 0; i < xfer_prev_op_count && i < xfer_queue.op_count; i += 1)
      {
        XFER_Op *op = &xfer_queue.ops[i];
        B32 started = (xfer_prev_status[i] == XFER_Status_Queued && op->status == XFER_Status_InProgress);
        B32 ended   = (xfer_prev_status[i] == XFER_Status_InProgress &&
                       (op->status == XFER_Status_Done || op->status == XFER_Status_Failed));
        if(started || ended)
        {
          if(op->kind == XFER_Kind_Download)
          {
            String8 dir = fs_path_parent(frame_arena, op->local_path);
            if(str8_match(dir, local_path)) { need_local_refresh = 1; }
          }
          else
          {
            String8 dir = sftp_path_parent(frame_arena, op->remote_path);
            if(str8_match(dir, rc.remote_path)) { need_remote_refresh = 1; }
          }
        }
      }
      if(need_local_refresh)
      {
        local_refresh_listing(local_arena, frame_arena, &local_path, &local_entries, &local_selected, &local_row_state);
      }
      if(need_remote_refresh && rc.connecting && !rc.control_joined)
      {
        remote_refresh_listing(frame_arena, &rc);
      }
    }

    // --- layout ---------------------------------------------------------------
    U32 win_w_u, win_h_u;
    wm_client_size(window, &win_w_u, &win_h_u);
    F32 win_w = (F32)win_w_u, win_h = (F32)win_h_u;

    // --- resizable divider handling ---------------------------------------------
    // Position-follows-mouse dragging (not delta-based) - simplest correct
    // resize-handle behavior, same "no cleverness" call as the rest of this
    // file. col_split_x/bottom_pane_h persist across frames (declared before
    // the loop) and are clamped every frame, not just while dragging, so a
    // window resize that would leave a stored split off-screen or too small
    // snaps back into range immediately.
    col_split_x = Clamp(MIN_PANE_W, col_split_x, Max(MIN_PANE_W, win_w - MIN_PANE_W));
    bottom_pane_h = Clamp(MIN_BOTTOM_H, bottom_pane_h, Max(MIN_BOTTOM_H, win_h - TOOLBAR_H - MIN_TOP_H));

    F32 mid_y0 = TOOLBAR_H;
    F32 mid_y1 = Max(mid_y0, win_h - bottom_pane_h);
    F32 bottom_y0 = mid_y1;

    {
      B32 modal_open = conn_modal.open || settings_modal.open || local_delete_confirm.open || remote_delete_confirm.open ||
                       host_key_confirm.open;
      B32 over_col_divider = !modal_open && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, col_split_x - DIVIDER_HIT_PAD, mid_y0, col_split_x + DIVIDER_HIT_PAD, mid_y1);
      B32 over_row_divider = !modal_open && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, bottom_y0 - DIVIDER_HIT_PAD, win_w, bottom_y0 + DIVIDER_HIT_PAD);
      B32 mouse_pressed_now  = ui_g_mouse_left_down && !ui_g_mouse_left_was_down;
      B32 mouse_released_now = !ui_g_mouse_left_down && ui_g_mouse_left_was_down;
      if(mouse_pressed_now && !col_split_dragging && !row_split_dragging)
      {
        if(over_col_divider)      { col_split_dragging = 1; }
        else if(over_row_divider) { row_split_dragging = 1; }
      }
      if(col_split_dragging)
      {
        col_split_x = Clamp(MIN_PANE_W, ui_g_mouse_x, Max(MIN_PANE_W, win_w - MIN_PANE_W));
        if(mouse_released_now) { col_split_dragging = 0; }
      }
      if(row_split_dragging)
      {
        bottom_pane_h = Clamp(MIN_BOTTOM_H, win_h - ui_g_mouse_y, Max(MIN_BOTTOM_H, win_h - TOOLBAR_H - MIN_TOP_H));
        if(mouse_released_now) { row_split_dragging = 0; }
      }
      // Re-derive so this frame's own drag is reflected immediately rather
      // than lagging a frame behind the mouse.
      mid_y1 = Max(mid_y0, win_h - bottom_pane_h);
      bottom_y0 = mid_y1;

      // Resize-cursor feedback - dragging pins it regardless of where the
      // mouse has since wandered (captured, same as the left button itself
      // - see WM_LBUTTONDOWN's SetCapture); otherwise it just follows hover.
      WM_CursorKind cursor_kind = WM_CursorKind_Arrow;
      if(col_split_dragging || (!row_split_dragging && over_col_divider)) { cursor_kind = WM_CursorKind_SizeWE; }
      else if(row_split_dragging || over_row_divider)                    { cursor_kind = WM_CursorKind_SizeNS; }
      wm_set_cursor(cursor_kind);
    }

    dr_begin_frame(0.08f, 0.08f, 0.10f, 1.0f);

    // Input gating while a modal is open: skip pane/tab/toolbar-action
    // interaction entirely (still draw them - just dimmed by the modal's
    // own overlay - don't act on clicks landing behind it). Declared before
    // the toolbar below since Disconnect needs it too, not just the panes.
    B32 input_gated = conn_modal.open || settings_modal.open ||
                       local_delete_confirm.open || remote_delete_confirm.open ||
                       host_key_confirm.open ||
                       col_split_dragging || row_split_dragging;

    // toolbar
    dr_rect(0, 0, win_w, TOOLBAR_H, 0.16f, 0.16f, 0.18f, 1.0f);
    dr_rect(0, TOOLBAR_H, win_w, TOOLBAR_H + 1.0f, 0.05f, 0.05f, 0.06f, 1.0f);
    {
      F32 conn_u0, conn_v0, conn_u1, conn_v1;
      ui_icon_conn_uv(&conn_u0, &conn_v0, &conn_u1, &conn_v1);
      // Input gating: while a modal is open, the toolbar toggle buttons
      // themselves stay live (so Close/re-toggle/switch-to-the-other-modal
      // all still work) - only the panes/tabs and action buttons like
      // Disconnect are blocked below.
      if(ui_icon_button(6.0f, 6.0f, TOOLBAR_H - 12.0f, conn_u0, conn_v0, conn_u1, conn_v1))
      {
        conn_modal.open = !conn_modal.open;
        if(conn_modal.open)
        {
          settings_modal.open = 0;
        }
      }
    }
    {
      F32 settings_u0, settings_v0, settings_u1, settings_v1;
      ui_icon_settings_uv(&settings_u0, &settings_v0, &settings_u1, &settings_v1);
      if(ui_icon_button(40.0f, 6.0f, TOOLBAR_H - 12.0f, settings_u0, settings_v0, settings_u1, settings_v1))
      {
        settings_modal.open = !settings_modal.open;
        if(settings_modal.open)
        {
          conn_modal.open = 0;
        }
      }
    }
    if(rc.active)
    {
      F32 dc_x = 74.0f, dc_y = 6.0f, dc_w = 96.0f, dc_h = TOOLBAR_H - 12.0f;
      B32 dc_hovered = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, dc_x, dc_y, dc_x + dc_w, dc_y + dc_h);
      F32 dc_bg = dc_hovered ? 0.70f : 0.55f;
      dr_rect(dc_x, dc_y, dc_x + dc_w, dc_y + dc_h, dc_bg, 0.12f, 0.12f, 1.0f);
      String8 dc_label = str8_lit("Disconnect");
      F32 dc_advance = 0.0f;
      FNT_Piece dc_measure[16];
      fnt_text_pieces(&font, 13.0f, dc_label, 0, 0, dc_measure, ArrayCount(dc_measure), &dc_advance);
      dr_text(&font, 13.0f, dc_x + (dc_w - dc_advance) * 0.5f, ui_text_baseline_y(dc_y, dc_h, 13.0f),
              1.0f, 1.0f, 1.0f, 1.0f, dc_label);
      if(!input_gated && dc_hovered && ui__mouse_pressed_edge())
      {
        // Covers both "actually connected" and "sitting in the
        // awaiting_reconnect gap" - teardown_live_session only makes
        // sense (and is only safe) when a thread is actually live;
        // the reconnect-timer path already left everything null'd
        // otherwise, matching the shutdown path's own guard below.
        if(!rc.control_joined)
        {
          teardown_live_session(&rc);
          rc.control_joined = 1;
          rc.session_arena = 0;
          rc.ring = 0;
          rc.in_ring = 0;
        }
        rc.awaiting_reconnect = 0;
        rc.active = 0;
        arena_clear(rc.remote_arena);
        rc.entry_count = 0;
        rc.sorted = 0;
        MemoryZeroArray(rc.selected);
        rc.row_state.last_click_index = -1;
        rc.row_state.shift_anchor_index = -1;
        str8_list_push(log_arena, &lines, str8_lit("disconnected"));
      }
    }

    // divider between the two panes - brightened while hovered/dragged so
    // the resize handle is discoverable despite being only DIVIDER_HIT_PAD*2
    // px wide
    {
      B32 col_hot = col_split_dragging ||
                    ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, col_split_x - DIVIDER_HIT_PAD, mid_y0, col_split_x + DIVIDER_HIT_PAD, mid_y1);
      F32 c = col_hot ? 0.35f : 0.05f;
      dr_rect(col_split_x - 1.0f, mid_y0, col_split_x + 1.0f, mid_y1, c, c, c, 1.0f);
    }
    // divider above the bottom pane - same affordance
    {
      B32 row_hot = row_split_dragging ||
                    ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, bottom_y0 - DIVIDER_HIT_PAD, win_w, bottom_y0 + DIVIDER_HIT_PAD);
      F32 c = row_hot ? 0.35f : 0.05f;
      dr_rect(0, bottom_y0, win_w, bottom_y0 + 1.0f, c, c, c, 1.0f);
    }
    // opaque bottom-pane backdrop - belt-and-suspenders alongside the tree
    // panes now being clipped to their own rects (see ui_row_list): nothing
    // above should ever be able to show through here regardless.
    dr_rect(0, bottom_y0 + 1.0f, win_w, win_h, 0.08f, 0.08f, 0.10f, 1.0f);

    // --- local pane -------------------------------------------------------------
    // Header shows the current directory (was a static "Local" label) -
    // clipped to the pane's own width so a long path can't bleed into the
    // remote pane, same reasoning ui_row_list already clips its rows for.
    dr_set_clip(6.0f, mid_y0, col_split_x - 6.0f, mid_y0 + 26.0f);
    dr_text(&font, 13.0f, 10.0f, ui_text_baseline_y(mid_y0, 26.0f, 13.0f), 0.55f, 0.55f, 0.55f, 1.0f, local_path);
    dr_clear_clip();
    F32 local_list_y = mid_y0 + 26.0f;
    F32 local_list_h = Max(0.0f, mid_y1 - local_list_y);
    B32 local_dbl = 0;
    S32 local_right_clicked = -1;
    F32 local_pane_w = col_split_x - 12.0f;
    S32 local_clicked = ui_row_list(frame_arena, &font, 6.0f, local_list_y, local_pane_w, local_list_h, ROW_H,
                                      local_entries.v, local_entries.count, local_selected, &local_row_state,
                                      &local_dbl, &local_right_clicked, input_gated);
    if(!input_gated && local_right_clicked >= 0)
    {
      local_ctx_menu.open = 1;
      local_ctx_menu.x = ui_g_mouse_x;
      local_ctx_menu.y = ui_g_mouse_y;
      local_ctx_menu.screen_w = win_w;
      local_ctx_menu.screen_h = win_h;
    }
    if(!input_gated && local_dbl && local_clicked >= 0 && (U64)local_clicked < local_entries.count)
    {
      FS_Entry *e = &local_entries.v[local_clicked];
      if(e->is_dir)
      {
        String8 new_path = str8_match(e->name, str8_lit(".."))
                          ? fs_path_parent(frame_arena, local_path)
                          : fs_path_join(frame_arena, local_path, e->name);
        arena_clear(local_arena);
        local_path = str8_copy(local_arena, new_path);
        local_entries = fs_list_dir(local_arena, local_path);
        local_selected = push_array(local_arena, B32, local_entries.count);
        local_row_state.last_click_index = -1;
        local_row_state.shift_anchor_index = -1;
        local_dir_store_save(local_path);
      }
    }

    // --- remote pane --------------------------------------------------------------
    // Header: host left-aligned (as before), current path right-aligned -
    // "." (the session's starting directory - see sftp_path_parent) reads
    // as "/" here since that's the top of what this pane can navigate to,
    // same clip-to-pane-width reasoning as the local header above.
    if(rc.active)
    {
      F32 header_x0 = col_split_x + 6.0f, header_x1 = win_w - 6.0f;
      dr_set_clip(header_x0, mid_y0, header_x1, mid_y0 + 26.0f);
      dr_text(&font, 13.0f, header_x0 + 4.0f, ui_text_baseline_y(mid_y0, 26.0f, 13.0f), 0.55f, 0.55f, 0.55f, 1.0f, rc.host);

      // "." (the session's starting directory itself) reads as "/"; any
      // real subdirectory of it (sftp_path_join never prefixes a "/" when
      // joining onto ".") gets one prefixed here so the leading slash
      // carries through navigation instead of only showing at the top.
      String8 display_path;
      if(str8_match(rc.remote_path, str8_lit("."))) { display_path = str8_lit("/"); }
      else if(rc.remote_path.size > 0 && rc.remote_path.str[0] == '/') { display_path = rc.remote_path; }
      else { display_path = str8f(frame_arena, "/%.*s", str8_varg(rc.remote_path)); }
      F32 path_advance = 0.0f;
      FNT_Piece path_measure[8];
      fnt_text_pieces(&font, 13.0f, display_path, 0, 0, path_measure, ArrayCount(path_measure), &path_advance);
      dr_text(&font, 13.0f, header_x1 - 4.0f - path_advance, ui_text_baseline_y(mid_y0, 26.0f, 13.0f), 0.55f, 0.55f, 0.55f, 1.0f, display_path);

      dr_clear_clip();
    }
    F32 remote_list_y = mid_y0 + 26.0f;
    F32 remote_list_h = Max(0.0f, mid_y1 - remote_list_y);
    F32 remote_pane_w = win_w - col_split_x - 12.0f; // was col_split_x-12 - wrong once the divider became draggable off-center
    B32 remote_dbl = 0;
    S32 remote_right_clicked = -1;
    S32 remote_clicked = ui_row_list(frame_arena, &font, col_split_x + 6.0f, remote_list_y, remote_pane_w, remote_list_h, ROW_H,
                                       rc.entries, rc.entry_count, rc.selected, &rc.row_state,
                                       &remote_dbl, &remote_right_clicked, input_gated);
    if(!input_gated && rc.connecting && !rc.control_joined && remote_right_clicked >= 0)
    {
      remote_ctx_menu.open = 1;
      remote_ctx_menu.x = ui_g_mouse_x;
      remote_ctx_menu.y = ui_g_mouse_y;
      remote_ctx_menu.screen_w = win_w;
      remote_ctx_menu.screen_h = win_h;
    }
    if(!input_gated && rc.connecting && !rc.control_joined && remote_dbl && remote_clicked >= 0 && (U64)remote_clicked < rc.entry_count)
    {
      FS_Entry *e = &rc.entries[remote_clicked];
      if(e->is_dir && !str8_match(e->name, str8_lit(".")))
      {
        String8 new_path = str8_match(e->name, str8_lit(".."))
                          ? sftp_path_parent(frame_arena, rc.remote_path)
                          : sftp_path_join(frame_arena, rc.remote_path, e->name);
        arena_clear(rc.remote_arena);
        rc.remote_path = str8_copy(rc.remote_arena, new_path);
        rc.entry_count = 0;
        rc.sorted = 0;
        MemoryZeroArray(rc.selected);
        rc.row_state.last_click_index = -1;
        rc.row_state.shift_anchor_index = -1;

        SFTP_Req req = {0};
        req.kind = SFTP_ReqKind_ListDir;
        req.path_size = Min(rc.remote_path.size, sizeof(req.path));
        MemoryCopy(req.path, rc.remote_path.str, req.path_size);
        guarded_ring_write_struct_or_wait(rc.in_ring, &req, max_U64);
      }
    }

    // --- drag-and-drop between the two panes -------------------------------------
    // Arm on a fresh press over a pane that already has a selection (rather
    // than requiring the press to land on an already-selected *row* -
    // whatever's currently selected in that pane is what gets dragged,
    // matching ordinary file-manager behavior); become a real drag only
    // past a small movement threshold so plain clicks never misfire as one;
    // on release over the *other* pane, queue one op per selected file.
    // A selected directory creates the same-named directory on the target
    // side and transfers everything inside it (see local_enqueue_folder_upload
    // for local->remote, and XFER_Scan/xfer_scan_start in xfer.h for the
    // remote->local direction, which needs an async remote tree walk).
    {
      F32 local_px0 = 6.0f, local_py0 = local_list_y, local_px1 = local_px0 + local_pane_w, local_py1 = local_py0 + local_list_h;
      F32 remote_px0 = col_split_x + 6.0f, remote_py0 = remote_list_y, remote_px1 = remote_px0 + remote_pane_w, remote_py1 = remote_py0 + remote_list_h;

      B32 mouse_pressed_now = ui_g_mouse_left_down && !ui_g_mouse_left_was_down;
      B32 mouse_released_now = !ui_g_mouse_left_down && ui_g_mouse_left_was_down;

      if(!input_gated)
      {
        if(mouse_pressed_now && !drag.active)
        {
          B32 in_local = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, local_px0, local_py0, local_px1, local_py1);
          B32 in_remote = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, remote_px0, remote_py0, remote_px1, remote_py1);
          B32 local_has_selection = 0;
          for(U64 i = 0; i < local_entries.count; i += 1) { if(local_selected[i]) { local_has_selection = 1; break; } }
          B32 remote_has_selection = 0;
          for(U64 i = 0; i < rc.entry_count; i += 1) { if(rc.selected[i]) { remote_has_selection = 1; break; } }

          if(in_local && local_has_selection)
          {
            drag.active = 1; drag.from_remote = 0; drag.start_x = ui_g_mouse_x; drag.start_y = ui_g_mouse_y;
          }
          else if(in_remote && remote_has_selection && rc.connecting && !rc.control_joined)
          {
            drag.active = 1; drag.from_remote = 1; drag.start_x = ui_g_mouse_x; drag.start_y = ui_g_mouse_y;
          }
        }

        if(drag.active && !drag.dragging)
        {
          F32 dx = ui_g_mouse_x - drag.start_x, dy = ui_g_mouse_y - drag.start_y;
          if(dx * dx + dy * dy > 16.0f) // ~4px movement threshold
          {
            drag.dragging = 1;
          }
        }

        if(drag.active && mouse_released_now)
        {
          if(drag.dragging)
          {
            B32 drop_in_local = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, local_px0, local_py0, local_px1, local_py1);
            B32 drop_in_remote = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, remote_px0, remote_py0, remote_px1, remote_py1);
            U64 enqueued_before = xfer_queue.op_count;
            B32 folder_op_started = 0;
            if(drag.from_remote && drop_in_local)
            {
              for(U64 i = 0; i < rc.entry_count; i += 1)
              {
                if(!rc.selected[i] || str8_match(rc.entries[i].name, str8_lit(".."))) { continue; }
                if(!sftp_name_is_safe_path_component(rc.entries[i].name))
                {
                  // A malicious or MITM-tampered server naming an entry
                  // e.g. "..\..\..\Startup\evil.exe" would otherwise land
                  // exactly there once naively joined onto local_path
                  // below - skip just this one entry, same as "..".
                  str8_list_push(log_arena, &lines,
                                  str8f(log_arena, "skipped '%.*s': unsafe name for a local path",
                                        (int)rc.entries[i].name.size, (char *)rc.entries[i].name.str));
                  continue;
                }
                String8 remote_full = sftp_path_join(frame_arena, rc.remote_path, rc.entries[i].name);
                String8 local_full = fs_path_join(frame_arena, local_path, rc.entries[i].name);
                if(rc.entries[i].is_dir)
                {
                  // The top-level directory is created right away (so it
                  // shows up even if the remote folder turns out to be
                  // empty); everything below it comes from the async scan
                  // - see xfer_scan_poll_all's per-frame drain, below.
                  fs_dir_create(local_full);
                  xfer_scan_start(folder_scans, ArrayCount(folder_scans), xfer_arena, local_full, remote_full,
                                   rc.host, rc.port, rc.user, rc.pass, rc.host_key_hash);
                  str8_list_push(log_arena, &lines,
                                  str8f(log_arena, "scanning remote folder '%.*s' for download...",
                                        (int)remote_full.size, (char *)remote_full.str));
                  folder_op_started = 1;
                }
                else
                {
                  xfer_enqueue(&xfer_queue, XFER_Kind_Download, local_full, remote_full, rc.entries[i].name,
                               rc.host, rc.port, rc.user, rc.pass, rc.host_key_hash, 0);
                }
              }
            }
            else if(!drag.from_remote && drop_in_remote && rc.connecting && !rc.control_joined)
            {
              for(U64 i = 0; i < local_entries.count; i += 1)
              {
                if(!local_selected[i] || str8_match(local_entries.v[i].name, str8_lit(".."))) { continue; }
                String8 local_full = fs_path_join(frame_arena, local_path, local_entries.v[i].name);
                String8 remote_full = sftp_path_join(frame_arena, rc.remote_path, local_entries.v[i].name);
                if(local_entries.v[i].is_dir)
                {
                  local_enqueue_folder_upload(frame_arena, &xfer_queue, local_full, remote_full,
                                               rc.host, rc.port, rc.user, rc.pass, rc.host_key_hash);
                }
                else
                {
                  xfer_enqueue(&xfer_queue, XFER_Kind_Upload, local_full, remote_full, local_entries.v[i].name,
                               rc.host, rc.port, rc.user, rc.pass, rc.host_key_hash, 0);
                }
              }
            }
            if(xfer_queue.op_count > enqueued_before || folder_op_started)
            {
              bottom_tab = 1; // Queue tab - a transfer just started, bring it into view
            }
          }
          drag.active = 0;
          drag.dragging = 0;
        }
      }
    }

    // --- bottom pane: tab strip + content ------------------------------------------
    S32 tab_clicked = ui_tab_strip(&font, 0, bottom_y0 + 1.0f, win_w / 4.0f, TAB_STRIP_H, tab_labels, 4, bottom_tab);
    if(!input_gated && tab_clicked >= 0)
    {
      bottom_tab = tab_clicked;
    }

    F32 content_y0 = bottom_y0 + 1.0f + TAB_STRIP_H;
    F32 content_h = Max(0.0f, win_h - content_y0);
    dr_set_clip(0, content_y0, win_w, win_h);
    switch(bottom_tab)
    {
      default: break;
      case 0: // Log - scrollable, auto-follows the latest line until the user scrolls
      {
        F32 line_h = 18.0f;
        F32 log_content_h = (F32)lines.node_count * line_h;
        F32 log_max_scroll = Max(0.0f, log_content_h - content_h);

        B32 mouse_in_log = !input_gated && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, content_y0, win_w, win_h);
        if(mouse_in_log)
        {
          F32 wheel = wm_mouse_wheel_delta();
          if(wheel != 0.0f)
          {
            log_auto_follow = 0;
            log_scroll_from_top -= wheel * line_h * 3.0f;
          }
        }
        if(log_auto_follow)
        {
          log_scroll_from_top = log_max_scroll;
        }
        log_scroll_from_top = Clamp(0.0f, log_scroll_from_top, log_max_scroll);
        if(log_scroll_from_top >= log_max_scroll)
        {
          log_auto_follow = 1; // scrolled back to the bottom - resume following
        }

        U64 idx = 0;
        F32 y_top = content_y0 + 4.0f - log_scroll_from_top;
        for(String8Node *n = lines.first; n != 0; n = n->next)
        {
          F32 line_y = y_top + (F32)idx * line_h;
          if(line_y + line_h >= content_y0 && line_y <= win_h)
          {
            dr_text(&font, 14.0f, 10.0f, ui_text_baseline_y(line_y, line_h, 14.0f), 0.8f, 0.8f, 0.8f, 1.0f, n->string);
          }
          idx += 1;
        }
      }break;
      case 1: // Queue - Queued + InProgress, with a proportional-fill progress bar
      {
        if(xfer_queue.drop_count > 0)
        {
          // XFER_MAX_OPS reached - queuing silently stopped otherwise;
          // this is the only thing that tells the user anything was
          // dropped (a large folder download is the realistic way to
          // hit this). Fixed position, doesn't participate in the
          // scrollable row list below.
          String8 warn = str8f(frame_arena, "warning: %llu file(s) could not be queued (transfer queue full)",
                                 xfer_queue.drop_count);
          dr_text(&font, 12.0f, 10.0f, ui_text_baseline_y(content_y0, 20.0f, 12.0f), 0.85f, 0.6f, 0.3f, 1.0f, warn);
        }
        F32 xr_h = 26.0f;
        U64 visible_count = 0;
        for(U64 i = 0; i < xfer_queue.op_count; i += 1)
        {
          XFER_Status st = xfer_queue.ops[i].status;
          if(st == XFER_Status_Queued || st == XFER_Status_InProgress) { visible_count += 1; }
        }
        F32 queue_max_scroll = Max(0.0f, (F32)visible_count * xr_h - content_h);
        B32 mouse_in_queue = !input_gated && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, content_y0, win_w, win_h);
        if(mouse_in_queue)
        {
          F32 wheel = wm_mouse_wheel_delta();
          if(wheel != 0.0f) { queue_scroll_y -= wheel * xr_h * 3.0f; }
        }
        queue_scroll_y = Clamp(0.0f, queue_scroll_y, queue_max_scroll);

        F32 xr_y0 = content_y0 + 4.0f - queue_scroll_y;
        U64 shown = 0;
        for(U64 i = 0; i < xfer_queue.op_count; i += 1)
        {
          XFER_Op *op = &xfer_queue.ops[i];
          if(op->status != XFER_Status_Queued && op->status != XFER_Status_InProgress)
          {
            continue;
          }
          F32 ry0 = xr_y0 + (F32)shown * xr_h;
          if(ry0 + xr_h < content_y0 || ry0 > win_h)
          {
            shown += 1;
            continue; // scrolled out of view - keep counting for row position, just don't draw/hit-test it
          }

          if(!input_gated && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, ry0, win_w, ry0 + xr_h) &&
             ui__mouse_right_pressed_edge())
          {
            xfer_ctx_menu.open = 1;
            xfer_ctx_menu.x = ui_g_mouse_x;
            xfer_ctx_menu.y = ui_g_mouse_y;
            xfer_ctx_menu.screen_w = (F32)win_w;
            xfer_ctx_menu.screen_h = (F32)win_h;
            xfer_ctx_menu.tag = op->id; // stable identity - see XFER_Op::id and xfer_cancel_by_id
          }

          String8 dir_glyph = (op->kind == XFER_Kind_Download) ? str8_lit("DL") : str8_lit("UL");
          dr_text(&font, 12.0f, 10.0f, ui_text_baseline_y(ry0, xr_h, 12.0f), 0.6f, 0.6f, 0.8f, 1.0f, dir_glyph);
          dr_text(&font, 13.0f, 40.0f, ui_text_baseline_y(ry0, xr_h, 13.0f), 0.85f, 0.85f, 0.85f, 1.0f, op->display_name);

          // Narrower than before (was win_w-110) to leave room for the ETA
          // column alongside the existing speed column to its right.
          F32 bar_x0 = 300.0f, bar_x1 = win_w - 200.0f, bar_y0 = ry0 + 6.0f, bar_y1 = ry0 + xr_h - 6.0f;
          dr_rect(bar_x0, bar_y0, bar_x1, bar_y1, 0.15f, 0.15f, 0.17f, 1.0f);
          if(op->status == XFER_Status_InProgress)
          {
            F32 frac = (op->size_total > 0) ? Clamp(0.0f, (F32)((F64)op->bytes_done / (F64)op->size_total), 1.0f) : 0.0f;
            dr_rect(bar_x0, bar_y0, bar_x0 + (bar_x1 - bar_x0) * frac, bar_y1, 0.25f, 0.45f, 0.65f, 1.0f);

            // percentage, centered over the bar itself
            String8 pct_str = str8f(frame_arena, "%u%%", (U32)(frac * 100.0f));
            F32 pct_advance = 0.0f;
            FNT_Piece pct_measure[8];
            fnt_text_pieces(&font, 11.0f, pct_str, 0, 0, pct_measure, ArrayCount(pct_measure), &pct_advance);
            F32 pct_x = bar_x0 + ((bar_x1 - bar_x0) - pct_advance) * 0.5f;
            dr_text(&font, 11.0f, pct_x, ui_text_baseline_y(ry0, xr_h, 11.0f), 0.95f, 0.95f, 0.95f, 1.0f, pct_str);

            // current speed (tight window - see xfer.c - not a slow-to-react
            // long average), to the right of the bar
            String8 speed_str = xfer_format_speed(frame_arena, op->current_bps);
            dr_text(&font, 12.0f, bar_x1 + 8.0f, ui_text_baseline_y(ry0, xr_h, 12.0f), 0.6f, 0.75f, 0.9f, 1.0f, speed_str);

            // estimated time to completion, from remaining bytes / current speed
            U64 bytes_remaining = (op->size_total > op->bytes_done) ? (op->size_total - op->bytes_done) : 0;
            String8 eta_str = xfer_format_eta(frame_arena, bytes_remaining, op->current_bps);
            dr_text(&font, 12.0f, win_w - 100.0f, ui_text_baseline_y(ry0, xr_h, 12.0f), 0.6f, 0.6f, 0.6f, 1.0f, eta_str);
          }
          else
          {
            dr_text(&font, 12.0f, bar_x0 + 6.0f, ui_text_baseline_y(ry0, xr_h, 12.0f), 0.55f, 0.55f, 0.55f, 1.0f, str8_lit("queued"));
          }

          if(op->status == XFER_Status_Queued)
          {
            if(ui_button(&font, win_w - 96.0f, ry0 + 2.0f, 86.0f, xr_h - 4.0f, str8_lit("Remove")))
            {
              xfer_cancel_queued(&xfer_queue, i);
              break; // indices shifted - resume next frame rather than iterate stale ones
            }
          }
          shown += 1;
        }
        if(shown == 0)
        {
          dr_text(&font, 14.0f, 10.0f, ui_text_baseline_y(content_y0, 26.0f, 14.0f), 0.5f, 0.5f, 0.5f, 1.0f, str8_lit("No transfers in progress."));
        }
      }break;
      case 2: // Failed
      {
        F32 xr_h = 26.0f;
        U64 visible_count = 0;
        for(U64 i = 0; i < xfer_queue.op_count; i += 1)
        {
          if(xfer_queue.ops[i].status == XFER_Status_Failed) { visible_count += 1; }
        }
        F32 failed_max_scroll = Max(0.0f, (F32)visible_count * xr_h - content_h);
        B32 mouse_in_failed = !input_gated && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, content_y0, win_w, win_h);
        if(mouse_in_failed)
        {
          F32 wheel = wm_mouse_wheel_delta();
          if(wheel != 0.0f) { failed_scroll_y -= wheel * xr_h * 3.0f; }
        }
        failed_scroll_y = Clamp(0.0f, failed_scroll_y, failed_max_scroll);

        F32 xr_y0 = content_y0 + 4.0f - failed_scroll_y;
        U64 shown = 0;
        for(U64 i = 0; i < xfer_queue.op_count; i += 1)
        {
          XFER_Op *op = &xfer_queue.ops[i];
          if(op->status != XFER_Status_Failed)
          {
            continue;
          }
          F32 ry0 = xr_y0 + (F32)shown * xr_h;
          if(ry0 + xr_h < content_y0 || ry0 > win_h)
          {
            shown += 1;
            continue;
          }
          dr_text(&font, 13.0f, 10.0f, ui_text_baseline_y(ry0, xr_h, 13.0f), 0.85f, 0.6f, 0.6f, 1.0f, op->display_name);
          dr_text(&font, 12.0f, 260.0f, ui_text_baseline_y(ry0, xr_h, 12.0f), 0.7f, 0.45f, 0.45f, 1.0f, op->error_text);
          if(ui_button(&font, win_w - 96.0f, ry0 + 2.0f, 86.0f, xr_h - 4.0f, str8_lit("Resume")))
          {
            xfer_resume(&xfer_queue, i);
            bottom_tab = 1; // Queue tab - same "just started a transfer" convention as a fresh drag-drop enqueue
          }
          shown += 1;
        }
        if(shown == 0)
        {
          dr_text(&font, 14.0f, 10.0f, ui_text_baseline_y(content_y0, 26.0f, 14.0f), 0.5f, 0.5f, 0.5f, 1.0f, str8_lit("No failed transfers."));
        }
      }break;
      case 3: // Completed
      {
        F32 xr_h = 26.0f;
        U64 visible_count = 0;
        for(U64 i = 0; i < xfer_queue.op_count; i += 1)
        {
          if(xfer_queue.ops[i].status == XFER_Status_Done) { visible_count += 1; }
        }
        F32 completed_max_scroll = Max(0.0f, (F32)visible_count * xr_h - content_h);
        B32 mouse_in_completed = !input_gated && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, 0, content_y0, win_w, win_h);
        if(mouse_in_completed)
        {
          F32 wheel = wm_mouse_wheel_delta();
          if(wheel != 0.0f) { completed_scroll_y -= wheel * xr_h * 3.0f; }
        }
        completed_scroll_y = Clamp(0.0f, completed_scroll_y, completed_max_scroll);

        F32 xr_y0 = content_y0 + 4.0f - completed_scroll_y;
        U64 shown = 0;
        for(U64 i = 0; i < xfer_queue.op_count; i += 1)
        {
          XFER_Op *op = &xfer_queue.ops[i];
          if(op->status != XFER_Status_Done)
          {
            continue;
          }
          F32 ry0 = xr_y0 + (F32)shown * xr_h;
          if(ry0 + xr_h < content_y0 || ry0 > win_h)
          {
            shown += 1;
            continue;
          }
          String8 dir_glyph = (op->kind == XFER_Kind_Download) ? str8_lit("DL") : str8_lit("UL");
          dr_text(&font, 12.0f, 10.0f, ui_text_baseline_y(ry0, xr_h, 12.0f), 0.6f, 0.8f, 0.6f, 1.0f, dir_glyph);
          dr_text(&font, 13.0f, 40.0f, ui_text_baseline_y(ry0, xr_h, 13.0f), 0.85f, 0.85f, 0.85f, 1.0f, op->display_name);
          String8 size_str = str8f(frame_arena, "%llu bytes", op->size_total);
          dr_text(&font, 12.0f, 300.0f, ui_text_baseline_y(ry0, xr_h, 12.0f), 0.55f, 0.55f, 0.55f, 1.0f, size_str);
          shown += 1;
        }
        if(shown == 0)
        {
          dr_text(&font, 14.0f, 10.0f, ui_text_baseline_y(content_y0, 26.0f, 14.0f), 0.5f, 0.5f, 0.5f, 1.0f, str8_lit("No completed transfers."));
        }
      }break;
    }
    dr_clear_clip();

    // --- Connections modal (drawn last so it's on top of everything else) -----------
    {
      String8 modal_host = {0}, modal_user = {0}, modal_pass = {0};
      U16 modal_port = 22;
      if(ui_conn_modal_update(&conn_modal, arena, rc.creds_arena, &events, &font, win_w, win_h,
                               &modal_host, &modal_port, &modal_user, &modal_pass))
      {
        connect_to(&rc, modal_host, modal_port, modal_user, modal_pass);
      }
    }
    ui_settings_modal_update(&settings_modal, &xfer_settings, &font, win_w, win_h);

    // Drag visual - drawn last (on top of the modal overlay too, though a
    // drag and a modal can't really coexist in practice) so it always
    // reads clearly regardless of what's under the cursor.
    if(drag.dragging)
    {
      U64 count = 0;
      if(drag.from_remote)
      {
        for(U64 i = 0; i < rc.entry_count; i += 1) { if(rc.selected[i] && !rc.entries[i].is_dir) { count += 1; } }
      }
      else
      {
        for(U64 i = 0; i < local_entries.count; i += 1) { if(local_selected[i] && !local_entries.v[i].is_dir) { count += 1; } }
      }
      String8 tag = str8f(frame_arena, "%llu item(s)", count);
      F32 tag_w = 110.0f, tag_h = 26.0f;
      F32 tag_x = ui_g_mouse_x + 14.0f, tag_y = ui_g_mouse_y + 14.0f;
      dr_rect(tag_x, tag_y, tag_x + tag_w, tag_y + tag_h, 0.25f, 0.45f, 0.65f, 0.92f);
      dr_text(&font, 13.0f, tag_x + 8.0f, ui_text_baseline_y(tag_y, tag_h, 13.0f), 1.0f, 1.0f, 1.0f, 1.0f, tag);
    }

    // Queue-row context menu - drawn/hit-tested last of all so it always
    // reads on top, same reasoning as the drag tag above.
    {
      S32 ctx_clicked = ui_context_menu(&font, &xfer_ctx_menu, xfer_ctx_menu_labels, ArrayCount(xfer_ctx_menu_labels));
      if(ctx_clicked == 0) // "Cancel Transfer" - the only item
      {
        xfer_cancel_by_id(&xfer_queue, xfer_ctx_menu.tag);
      }
    }

    // Local/remote pane row context menu ("Delete") - choosing it opens a
    // confirm dialog rather than deleting immediately (see below); the
    // message names the single selected file, or a count for a multi-
    // selection. Directories in the selection are silently skipped (not
    // supported yet - see fs_delete_file/SFTP_ReqKind_Delete) - if that
    // leaves nothing eligible, the menu choice is a no-op.
    {
      S32 ctx_clicked = ui_context_menu(&font, &local_ctx_menu, delete_ctx_menu_labels, ArrayCount(delete_ctx_menu_labels));
      if(ctx_clicked == 0) // "Delete" - the only item
      {
        U64 file_count = 0;
        String8 single_name = {0};
        for(U64 i = 0; i < local_entries.count; i += 1)
        {
          if(local_selected[i] && !local_entries.v[i].is_dir)
          {
            if(file_count == 0) { single_name = local_entries.v[i].name; }
            file_count += 1;
          }
        }
        if(file_count > 0)
        {
          String8 message = (file_count == 1)
            ? str8f(frame_arena, "Delete \"%.*s\"?", (int)single_name.size, (char *)single_name.str)
            : str8f(frame_arena, "Delete %llu items?", file_count);
          ui_confirm_open(&local_delete_confirm, message);
        }
      }
    }
    {
      S32 ctx_clicked = ui_context_menu(&font, &remote_ctx_menu, delete_ctx_menu_labels, ArrayCount(delete_ctx_menu_labels));
      if(ctx_clicked == 0) // "Delete" - the only item
      {
        U64 file_count = 0;
        String8 single_name = {0};
        for(U64 i = 0; i < rc.entry_count; i += 1)
        {
          if(rc.selected[i] && !rc.entries[i].is_dir)
          {
            if(file_count == 0) { single_name = rc.entries[i].name; }
            file_count += 1;
          }
        }
        if(file_count > 0)
        {
          String8 message = (file_count == 1)
            ? str8f(frame_arena, "Delete \"%.*s\"?", (int)single_name.size, (char *)single_name.str)
            : str8f(frame_arena, "Delete %llu items?", file_count);
          ui_confirm_open(&remote_delete_confirm, message);
        }
      }
    }

    // Local delete confirm - fires the actual DeleteFileW calls and
    // re-lists in place, same pattern local_refresh_listing already serves
    // for transfer completion.
    if(ui_confirm_dialog(&font, &local_delete_confirm, win_w, win_h, str8_lit("Delete")))
    {
      for(U64 i = 0; i < local_entries.count; i += 1)
      {
        if(local_selected[i] && !local_entries.v[i].is_dir)
        {
          String8 full_path = fs_path_join(frame_arena, local_path, local_entries.v[i].name);
          fs_delete_file(full_path);
        }
      }
      local_refresh_listing(local_arena, frame_arena, &local_path, &local_entries, &local_selected, &local_row_state);
    }

    // Remote delete confirm - queues one SFTP_ReqKind_Delete per selected
    // file, then one ListDir refresh (remote_refresh_listing) - the control
    // thread processes its in_ring strictly in order, so the refresh always
    // lists what's left after every delete above it has actually happened.
    if(ui_confirm_dialog(&font, &remote_delete_confirm, win_w, win_h, str8_lit("Delete")))
    {
      if(rc.connecting && !rc.control_joined)
      {
        for(U64 i = 0; i < rc.entry_count; i += 1)
        {
          if(rc.selected[i] && !rc.entries[i].is_dir)
          {
            String8 full_path = sftp_path_join(frame_arena, rc.remote_path, rc.entries[i].name);
            SFTP_Req req = {0};
            req.kind = SFTP_ReqKind_Delete;
            req.path_size = Min(full_path.size, sizeof(req.path));
            MemoryCopy(req.path, full_path.str, req.path_size);
            guarded_ring_write_struct_or_wait(rc.in_ring, &req, max_U64);
          }
        }
        remote_refresh_listing(frame_arena, &rc);
      }
    }

    // Host key confirmation - resolves the prompt opened from the
    // SFTP_CtrlMsgKind_HostKeyVerify branch above. Called unconditionally
    // every frame (like the delete confirms above), not just while
    // rc.host_key_prompt_pending is set, so Trust/Cancel can still close
    // the dialog even in the rare case connect_to() switched targets (and
    // so reset host_key_prompt_pending to 0 - see its own comment) while
    // this was still up - otherwise it would stay open with nothing left
    // to ever resolve it, permanently gating input via modal_open/
    // input_gated above.
    //
    // ui_confirm_dialog only reports the Trust click directly; Cancel
    // just closes the dialog and returns 0 (see its own doc comment), so
    // a Reject is detected the same way as everywhere else in this
    // codebase that needs to tell "cancelled" apart from "still open, no
    // decision yet": snapshot `open` before the call, and diff after.
    {
      B32 was_open = host_key_confirm.open;
      B32 trusted = ui_confirm_dialog(&font, &host_key_confirm, win_w, win_h, str8_lit("Trust"));
      B32 rejected = was_open && !host_key_confirm.open && !trusted;
      if(rc.host_key_prompt_pending && (trusted || rejected))
      {
        // The control thread is blocked waiting for exactly one of these
        // on rc.in_ring - see sftp_client.c's host-key-verify gate.
        SFTP_Req req = {0};
        req.kind = trusted ? SFTP_ReqKind_HostKeyAccept : SFTP_ReqKind_HostKeyReject;
        guarded_ring_write_struct_or_wait(rc.in_ring, &req, max_U64);
        rc.host_key_prompt_pending = 0;
        if(trusted)
        {
          MemoryCopy(rc.host_key_hash, rc.host_key_prompt_hash, SFTP_HOST_KEY_HASH_SIZE);
          rc.has_host_key_hash = 1;
          kh_store_set(&rc.kh_hosts, arena, rc.host, rc.port, rc.host_key_hash);
          kh_store_save(&rc.kh_hosts);
        }
      }
    }

    dr_end_frame();
    ui_end_frame();
  }

  B32 clean_shutdown = 1;
  if(rc.connecting && !rc.control_joined)
  {
    clean_shutdown = teardown_live_session(&rc);
  }
  // else: either never connected, or currently sitting in the
  // awaiting_reconnect gap between sessions - no live thread either way,
  // nothing to signal or join.
  //
  // If teardown timed out, the control thread was detached (still
  // running, possibly still inside a libssh2 call on this session) rather
  // than actually joined - calling libssh2_exit() right out from under it
  // would race that thread's own libssh2 calls against global library
  // teardown. Skip it and let the OS reclaim everything on process exit
  // instead, which is safe either way.
  if(clean_shutdown)
  {
    libssh2_exit();
  }

  return 0;
}
