#include <wincrypt.h>
#include <dpapi.h>
#pragma comment(lib, "crypt32")

// Builds "%APPDATA%\FTP-FTW\connections.dat" as a null-terminated wide
// string (needed for CreateFileW/CreateDirectoryW), creating the
// containing directory along the way. Returns 0 if APPDATA isn't set
// (shouldn't happen on real Windows, but nothing to do about it if so).
// `out_path` is allocated from `arena`, which must outlive its use.
internal B32
conn_store__get_path(Arena *arena, WCHAR **out_path)
{
  char *appdata = getenv("APPDATA");
  if(appdata == 0)
  {
    return 0;
  }

  Temp scratch = scratch_begin(&arena, 1);
  String8 appdata_s8 = str8_cstring(appdata);
  String8 dir_s8 = str8f(scratch.arena, "%.*s\\FTP-FTW", str8_varg(appdata_s8));
  String8 file_s8 = str8f(scratch.arena, "%.*s\\connections.dat", str8_varg(dir_s8));

  int dir_wide_count = MultiByteToWideChar(CP_UTF8, 0, (char *)dir_s8.str, (int)dir_s8.size, 0, 0);
  WCHAR *dir_w = push_array_no_zero(scratch.arena, WCHAR, dir_wide_count + 1);
  MultiByteToWideChar(CP_UTF8, 0, (char *)dir_s8.str, (int)dir_s8.size, dir_w, dir_wide_count);
  dir_w[dir_wide_count] = 0;
  CreateDirectoryW(dir_w, 0); // fine if it already exists - failure ignored either way

  int file_wide_count = MultiByteToWideChar(CP_UTF8, 0, (char *)file_s8.str, (int)file_s8.size, 0, 0);
  WCHAR *file_w = push_array_no_zero(arena, WCHAR, file_wide_count + 1); // caller-owned lifetime
  MultiByteToWideChar(CP_UTF8, 0, (char *)file_s8.str, (int)file_s8.size, file_w, file_wide_count);
  file_w[file_wide_count] = 0;

  *out_path = file_w;
  scratch_end(scratch);
  return 1;
}

internal U8 *
conn_store__serialize(Arena *arena, CONN_ProfileArray *profiles, U64 *out_size)
{
  U64 count = profiles->count;

  // Compute the exact size needed up front rather than assuming some
  // fixed capacity is "surely enough" - text fields are user-entered
  // (up to 255 bytes each, per UI_TextEditState's own cap) and
  // CONN_MAX_PROFILES is 64, so 4 maxed-out fields across every profile
  // can legitimately need ~67KB: a fixed 64KB buffer here previously
  // overflowed by ~2KB through completely normal use (many saved
  // profiles with long passwords/hostnames), corrupting whatever the
  // arena placed right after it.
  U64 needed = sizeof(count);
  for(U64 i = 0; i < count; i += 1)
  {
    CONN_Profile *p = &profiles->v[i];
    needed += sizeof(p->port);
    String8 *fields[4] = { &p->name, &p->host, &p->username, &p->password };
    for(U64 f = 0; f < 4; f += 1)
    {
      needed += sizeof(U64) + fields[f]->size;
    }
  }

  U8 *buf = push_array_no_zero(arena, U8, needed);
  U64 pos = 0;

  MemoryCopy(buf + pos, &count, sizeof(count)); pos += sizeof(count);

  for(U64 i = 0; i < count; i += 1)
  {
    CONN_Profile *p = &profiles->v[i];
    MemoryCopy(buf + pos, &p->port, sizeof(p->port)); pos += sizeof(p->port);

    String8 *fields[4] = { &p->name, &p->host, &p->username, &p->password };
    for(U64 f = 0; f < 4; f += 1)
    {
      U64 sz = fields[f]->size;
      MemoryCopy(buf + pos, &sz, sizeof(sz)); pos += sizeof(sz);
      MemoryCopy(buf + pos, fields[f]->str, sz); pos += sz;
    }
  }

  *out_size = pos;
  return buf;
}

// Bounds-checked against a truncated/corrupt file - this is untrusted
// input from disk (could be hand-edited, disk corruption, a foreign
// file), not something we produced and can trust blindly.
internal CONN_ProfileArray
conn_store__deserialize(Arena *arena, U8 *buf, U64 size)
{
  CONN_ProfileArray result = {0};
  U64 pos = 0;

  if(pos + sizeof(U64) > size)
  {
    return result;
  }
  U64 count = 0;
  MemoryCopy(&count, buf + pos, sizeof(count)); pos += sizeof(count);
  count = Min(count, (U64)CONN_MAX_PROFILES);

  for(U64 i = 0; i < count; i += 1)
  {
    if(pos + sizeof(U16) > size)
    {
      break;
    }
    CONN_Profile *p = &result.v[result.count];
    MemoryCopy(&p->port, buf + pos, sizeof(p->port)); pos += sizeof(p->port);

    String8 *fields[4] = { &p->name, &p->host, &p->username, &p->password };
    B32 ok = 1;
    for(U64 f = 0; f < 4; f += 1)
    {
      if(pos + sizeof(U64) > size)
      {
        ok = 0;
        break;
      }
      U64 sz = 0;
      MemoryCopy(&sz, buf + pos, sizeof(sz)); pos += sizeof(sz);
      if(pos + sz > size)
      {
        ok = 0;
        break;
      }
      *fields[f] = str8_copy(arena, str8(buf + pos, sz));
      pos += sz;
    }
    if(!ok)
    {
      break;
    }
    result.count += 1;
  }

  return result;
}

internal CONN_ProfileArray
conn_store_load(Arena *arena)
{
  CONN_ProfileArray result = {0};
  Temp scratch = scratch_begin(&arena, 1);

  WCHAR *path = 0;
  if(!conn_store__get_path(scratch.arena, &path))
  {
    scratch_end(scratch);
    return result;
  }

  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if(file == INVALID_HANDLE_VALUE)
  {
    scratch_end(scratch); // no file yet - first run, not an error
    return result;
  }

  LARGE_INTEGER size_li = {0};
  GetFileSizeEx(file, &size_li);
  U64 file_size = (U64)size_li.QuadPart;
  B32 read_ok = 1;
  U8 *encrypted = 0;
  if(file_size > 0)
  {
    encrypted = push_array_no_zero(scratch.arena, U8, file_size);
    DWORD bytes_read = 0;
    read_ok = ReadFile(file, encrypted, (DWORD)file_size, &bytes_read, 0) && bytes_read == file_size;
  }
  CloseHandle(file);

  if(read_ok && file_size > 0)
  {
    DATA_BLOB in_blob = { (DWORD)file_size, encrypted };
    DATA_BLOB out_blob = {0};
    if(CryptUnprotectData(&in_blob, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out_blob))
    {
      result = conn_store__deserialize(arena, out_blob.pbData, out_blob.cbData);
      LocalFree(out_blob.pbData);
    }
  }

  scratch_end(scratch);
  return result;
}

internal void
conn_store_save(CONN_ProfileArray *profiles)
{
  Temp scratch = scratch_begin(0, 0);

  WCHAR *path = 0;
  if(!conn_store__get_path(scratch.arena, &path))
  {
    scratch_end(scratch);
    return;
  }

  U64 plain_size = 0;
  U8 *plain = conn_store__serialize(scratch.arena, profiles, &plain_size);

  DATA_BLOB in_blob = { (DWORD)plain_size, plain };
  DATA_BLOB out_blob = {0};
  if(CryptProtectData(&in_blob, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &out_blob))
  {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if(file != INVALID_HANDLE_VALUE)
    {
      DWORD bytes_written = 0;
      WriteFile(file, out_blob.pbData, out_blob.cbData, &bytes_written, 0);
      CloseHandle(file);
    }
    LocalFree(out_blob.pbData);
  }

  scratch_end(scratch);
}
