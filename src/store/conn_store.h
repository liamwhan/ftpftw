// Saved SFTP connection profiles (CONN_ namespace), persisted to
// %APPDATA%\lwftpclient\connections.dat with the whole file encrypted via
// Windows DPAPI (CryptProtectData) - scoped to the current Windows user
// account (default flags, no CRYPTPROTECT_LOCAL_MACHINE), no separate
// master password. This protects the file at rest (copied elsewhere, read
// by another tool); it does not protect against other software already
// running as this same logged-in user, which is an inherent limit of any
// local-only secret store, not specific to DPAPI.
//
// On-disk format (see conn_store.c): a simple length-prefixed binary
// layout - record count, then per record a U16 port followed by four
// (U64 size + raw bytes) fields. No delimiters to escape, no JSON/ini
// parser needed for what's realistically a handful of records.

#ifndef CONN_STORE_H
#define CONN_STORE_H

#define CONN_MAX_PROFILES 64

typedef struct CONN_Profile CONN_Profile;
struct CONN_Profile
{
  String8 name;
  String8 host;
  U16 port;
  String8 username;
  String8 password;
};

typedef struct CONN_ProfileArray CONN_ProfileArray;
struct CONN_ProfileArray
{
  CONN_Profile v[CONN_MAX_PROFILES];
  U64 count;
};

// Reads and DPAPI-decrypts the connections file, if present. A missing
// file (first run) is not an error - just an empty array. Profile strings
// are allocated from `arena`, which must outlive the returned array (the
// caller's app-lifetime arena - never an arena that gets cleared/released
// while the array is still displayed/editable).
internal CONN_ProfileArray conn_store_load(Arena *arena);

// Serializes and DPAPI-encrypts `profiles`, writing to the connections
// file (creating the containing directory first if needed).
internal void conn_store_save(CONN_ProfileArray *profiles);

#endif // CONN_STORE_H
