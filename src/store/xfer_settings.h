// Transfer concurrency settings (max simultaneous downloads/uploads),
// persisted to %APPDATA%\lwftpclient\settings.dat as two plain U32s - not
// secrets, no DPAPI needed (contrast with conn_store.c's encrypted
// connection profiles).

#ifndef XFER_SETTINGS_H
#define XFER_SETTINGS_H

#define XFER_SETTINGS_HARD_CAP 10

typedef struct XFER_Settings XFER_Settings;
struct XFER_Settings
{
  U32 max_downloads;
  U32 max_uploads;
};

// Loads from disk, clamping both values to [1, XFER_SETTINGS_HARD_CAP] -
// covers a missing (first-run), truncated, or hand-edited file. Defaults
// to {2, 2} if nothing's on disk yet.
internal XFER_Settings xfer_settings_load(void);

// Serializes and writes to disk (creating the containing directory first
// if needed), clamping both values on the way out too.
internal void xfer_settings_save(XFER_Settings *settings);

#endif // XFER_SETTINGS_H
