// Transfer settings modal (UI_ namespace): max simultaneous downloads/
// uploads. Same purpose-built-not-generic pattern as ui_conn_modal.h - its
// own small state, not a shared modal-stack system.

#ifndef UI_SETTINGS_MODAL_H
#define UI_SETTINGS_MODAL_H

typedef struct UI_SettingsModal UI_SettingsModal;
struct UI_SettingsModal
{
  B32 open;
};

internal void ui_settings_modal_init(UI_SettingsModal *modal);

// Draws and handles input for the modal iff `modal->open` (a no-op
// otherwise). The +/- steppers write straight into `*settings` and
// persist immediately via xfer_settings_save on every change - no
// separate Save button, since persisting two integers is cheap enough to
// just do on every click rather than batching.
internal void ui_settings_modal_update(UI_SettingsModal *modal, XFER_Settings *settings,
                                        FP_Font *font, F32 win_w, F32 win_h);

#endif // UI_SETTINGS_MODAL_H
