// Last local-pane directory, persisted as the raw UTF-8 path bytes to
// %APPDATA%\FTP-FTW\local_dir.dat - not a secret (contrast with
// conn_store.c's encrypted connection profiles), same "plain file, no
// DPAPI" call as xfer_settings.c's settings.dat.

#ifndef LOCAL_DIR_STORE_H
#define LOCAL_DIR_STORE_H

#define LOCAL_DIR_STORE_MAX_SIZE KB(4) // generous for any real filesystem path

// Reads the saved path into `arena`. Returns an empty String8 if nothing's
// saved yet (first run) or the file can't be read - the caller falls back
// to its own default (main.c falls back to %USERPROFILE%).
internal String8 local_dir_store_load(Arena *arena);

// Serializes and writes to disk (creating the containing directory first
// if needed), truncated to LOCAL_DIR_STORE_MAX_SIZE if somehow longer.
internal void local_dir_store_save(String8 path);

#endif // LOCAL_DIR_STORE_H
