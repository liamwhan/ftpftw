// The Connections modal (UI_ namespace): a rendered (not Win32) dialog for
// managing saved SFTP profiles, backed by conn_store's DPAPI-encrypted
// file. Purpose-built for this one screen - not a generic modal-stack
// system, since nothing else needs modals yet.

#ifndef UI_CONN_MODAL_H
#define UI_CONN_MODAL_H

typedef struct UI_ConnModal UI_ConnModal;
struct UI_ConnModal
{
  B32 open;
  CONN_ProfileArray profiles;
  S32 selected_index; // which saved profile is highlighted in the list, -1 = none/new
  S32 focused_field;   // 0=name,1=host,2=port,3=username,4=password, -1=none
  UI_TextEditState name, host, port, username, password;
};

// Loads saved profiles from disk into `persist_arena` (the app-lifetime
// arena - profile strings live here for as long as the modal exists, so
// this must never be an arena that gets cleared/released, e.g. not a
// per-session or per-connect arena).
internal void ui_conn_modal_init(UI_ConnModal *modal, Arena *persist_arena);

// Draws and handles input for the modal iff `modal->open` (a no-op
// otherwise). Returns 1 the frame the user clicks Connect, filling
// *out_host/out_port/out_user/out_pass - copied into `creds_arena`, which
// the caller should treat the same way as any other value it hands to
// start_sftp_session (must outlive the connection attempt).
internal B32 ui_conn_modal_update(UI_ConnModal *modal, Arena *persist_arena, Arena *creds_arena,
                                    WM_EventList *events, FP_Font *font, F32 win_w, F32 win_h,
                                    String8 *out_host, U16 *out_port, String8 *out_user, String8 *out_pass);

#endif // UI_CONN_MODAL_H
