// Local filesystem listing (FS_ namespace). Windows-specific code directly
// in this file - no os/-split needed, same as sftp/ already being direct
// platform code. FS_Entry is also the shared row shape the UI's row-list
// widget renders for BOTH the local pane (from here) and the remote pane
// (converted from SFTP_CtrlMsg Entry messages in main.c) - one draw/hit-test
// path serves both.

#ifndef FS_LOCAL_H
#define FS_LOCAL_H

typedef struct FS_Entry FS_Entry;
struct FS_Entry
{
  String8 name;
  B32 is_dir;
  U64 size;
};

typedef struct FS_EntryArray FS_EntryArray;
struct FS_EntryArray
{
  FS_Entry *v;
  U64 count;
};

// Directories first, then alphabetical within each group - shared by local
// listing here and remote-listing sort in main.c (qsort comparator).
internal int fs_entry_compare(const void *a, const void *b);

// Lists `path` (no recursion - one level). Includes ".." unless `path` is
// already a drive root (see fs_path_is_root), so the local pane can
// navigate up; excludes ".".
internal FS_EntryArray fs_list_dir(Arena *arena, String8 path);

internal String8 fs_path_join(Arena *arena, String8 dir, String8 name);
internal String8 fs_path_parent(Arena *arena, String8 path);
internal B32      fs_path_is_root(String8 path); // e.g. "C:\"

// True if `path` exists and is a directory - used to validate a
// previously-saved local pane path (see local_dir_store.h) at startup
// before navigating into it, since it may have since been moved/deleted.
internal B32 fs_dir_exists(String8 path);

// Deletes a single file (DeleteFileW) - directories aren't supported yet,
// matching the drag-drop transfer path's own file-only scope (see main.c's
// drag-and-drop block: "directories are skipped this pass"). Returns
// whether the delete actually succeeded.
internal B32 fs_delete_file(String8 path);

// Creates `path` and any missing intermediate directories (mkdir -p
// semantics) - used by the folder-drag download path so a downloaded
// file's local parent directory just gets created on the spot instead of
// requiring a separate up-front directory-tree walk. Returns whether
// `path` exists as a directory when this returns (true also when it
// already existed).
internal B32 fs_dir_create(String8 path);

// Local file I/O for the transfer engine (src/xfer/) - opaque handle,
// matches the WM_Window/Thread convention. Streaming (chunk at a time) -
// never load a whole file into memory.
typedef struct FS_File FS_File;
struct FS_File
{
  U64 u64[1]; // opaque - wraps a HANDLE
};

internal FS_File fs_file_open_read(String8 path);
internal FS_File fs_file_open_write(String8 path); // create/truncate
internal FS_File fs_file_open_write_keep(String8 path); // create if missing, but don't truncate if it exists - for resume
internal B32     fs_file_is_valid(FS_File file);
internal U64     fs_file_read(FS_File file, void *buf, U64 max_size);  // bytes read, 0 = EOF or error
internal B32     fs_file_write(FS_File file, void *buf, U64 size);     // writes all `size` bytes or fails
internal B32     fs_file_seek(FS_File file, U64 offset); // absolute, from the start of the file
internal void    fs_file_close(FS_File file);
internal B32     fs_file_size(String8 path, U64 *out_size);

#endif // FS_LOCAL_H
