// Pinned SSH host key fingerprints (KH_ namespace), persisted as plain
// (unencrypted) bytes to %APPDATA%\FTP-FTW\known_hosts.dat - a host
// key hash isn't a secret (it's meant to be compared, potentially out of
// band, the same way OpenSSH's own known_hosts file is never encrypted),
// so this deliberately skips conn_store.c's DPAPI step.
//
// Keyed by (host, port) rather than by saved CONN_Profile, on purpose:
// a connection doesn't have to come from a saved profile (the Connections
// modal's Connect button works from ad-hoc typed-in fields too), and the
// trust decision is really about the *endpoint*, not whatever name the
// user happened to file it under - this mirrors how SSH known_hosts
// itself is keyed by hostname, never by some higher-level "profile"
// concept.
//
// On-disk format: record count, then per record a length-prefixed host
// string, a U16 port, and a fixed 32-byte SHA256 host key hash - same
// simple length-prefixed binary layout conn_store.c uses, bounds-checked
// against truncated/corrupt data on load for the same reason (untrusted
// disk input, not something we can assume we produced ourselves intact).

#ifndef KNOWN_HOSTS_STORE_H
#define KNOWN_HOSTS_STORE_H

#define KH_MAX_HOSTS 256
#define KH_HASH_SIZE 32

typedef struct KH_Host KH_Host;
struct KH_Host
{
  String8 host;
  U16 port;
  U8 hash[KH_HASH_SIZE];
};

typedef struct KH_HostArray KH_HostArray;
struct KH_HostArray
{
  KH_Host v[KH_MAX_HOSTS];
  U64 count;
};

// Reads the known-hosts file, if present. A missing file (first run) is
// not an error - just an empty array. Host strings are allocated from
// `arena`, which must outlive the returned array (the caller's
// app-lifetime arena, same contract as conn_store_load).
internal KH_HostArray kh_store_load(Arena *arena);

// Serializes and writes `hosts` to disk (creating the containing
// directory first if needed).
internal void kh_store_save(KH_HostArray *hosts);

// Looks up the pinned fingerprint for (host, port), case-sensitive exact
// match (hostnames as typed/saved, no normalization - matches how they're
// compared everywhere else in this codebase, e.g. RemoteConn.host).
// Returns 0 (out_hash left untouched) if nothing is pinned for this
// endpoint yet.
internal B32 kh_store_find(KH_HostArray *hosts, String8 host, U16 port, U8 out_hash[KH_HASH_SIZE]);

// Inserts or updates the pinned fingerprint for (host, port). `arena`
// must be the same app-lifetime arena `hosts` was loaded into (a fresh
// host string is copied into it). No-op if the table is already full and
// this would be a brand new entry (existing entries can still be
// updated) - same "just drop it" precedent as CONN_MAX_PROFILES.
internal void kh_store_set(KH_HostArray *hosts, Arena *arena, String8 host, U16 port, U8 hash[KH_HASH_SIZE]);

#endif // KNOWN_HOSTS_STORE_H
