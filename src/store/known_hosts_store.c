// Builds "%APPDATA%\FTP-FTW\known_hosts.dat" - same pattern as
// conn_store__get_path/local_dir_store__get_path/xfer_settings__get_path
// (one copy per store rather than a shared helper - four call sites still
// doesn't earn an abstraction).
internal B32
kh_store__get_path(Arena *arena, WCHAR **out_path)
{
  char *appdata = getenv("APPDATA");
  if(appdata == 0)
  {
    return 0;
  }

  Temp scratch = scratch_begin(&arena, 1);
  String8 appdata_s8 = str8_cstring(appdata);
  String8 dir_s8 = str8f(scratch.arena, "%.*s\\FTP-FTW", str8_varg(appdata_s8));
  String8 file_s8 = str8f(scratch.arena, "%.*s\\known_hosts.dat", str8_varg(dir_s8));

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
kh_store__serialize(Arena *arena, KH_HostArray *hosts, U64 *out_size)
{
  U64 count = hosts->count;

  // Exact size up front, same reasoning as conn_store__serialize (see its
  // own comment) - host names are short in practice but nothing bounds
  // them below CONN_Profile's own 255-byte field cap, so don't assume a
  // fixed buffer is "surely enough".
  U64 needed = sizeof(count);
  for(U64 i = 0; i < count; i += 1)
  {
    needed += sizeof(U64) + hosts->v[i].host.size + sizeof(hosts->v[i].port) + KH_HASH_SIZE;
  }

  U8 *buf = push_array_no_zero(arena, U8, needed);
  U64 pos = 0;
  MemoryCopy(buf + pos, &count, sizeof(count)); pos += sizeof(count);
  for(U64 i = 0; i < count; i += 1)
  {
    KH_Host *h = &hosts->v[i];
    U64 sz = h->host.size;
    MemoryCopy(buf + pos, &sz, sizeof(sz)); pos += sizeof(sz);
    MemoryCopy(buf + pos, h->host.str, sz); pos += sz;
    MemoryCopy(buf + pos, &h->port, sizeof(h->port)); pos += sizeof(h->port);
    MemoryCopy(buf + pos, h->hash, KH_HASH_SIZE); pos += KH_HASH_SIZE;
  }

  *out_size = pos;
  return buf;
}

// Bounds-checked against a truncated/corrupt file - untrusted disk input,
// same reasoning as conn_store__deserialize.
internal KH_HostArray
kh_store__deserialize(Arena *arena, U8 *buf, U64 size)
{
  KH_HostArray result = {0};
  U64 pos = 0;

  if(pos + sizeof(U64) > size)
  {
    return result;
  }
  U64 count = 0;
  MemoryCopy(&count, buf + pos, sizeof(count)); pos += sizeof(count);
  count = Min(count, (U64)KH_MAX_HOSTS);

  for(U64 i = 0; i < count; i += 1)
  {
    if(pos + sizeof(U64) > size)
    {
      break;
    }
    U64 sz = 0;
    MemoryCopy(&sz, buf + pos, sizeof(sz)); pos += sizeof(sz);
    if(pos + sz + sizeof(U16) + KH_HASH_SIZE > size)
    {
      break;
    }
    KH_Host *h = &result.v[result.count];
    h->host = str8_copy(arena, str8(buf + pos, sz)); pos += sz;
    MemoryCopy(&h->port, buf + pos, sizeof(h->port)); pos += sizeof(h->port);
    MemoryCopy(h->hash, buf + pos, KH_HASH_SIZE); pos += KH_HASH_SIZE;
    result.count += 1;
  }

  return result;
}

internal KH_HostArray
kh_store_load(Arena *arena)
{
  KH_HostArray result = {0};
  Temp scratch = scratch_begin(&arena, 1);

  WCHAR *path = 0;
  if(!kh_store__get_path(scratch.arena, &path))
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
  U8 *buf = 0;
  if(file_size > 0)
  {
    buf = push_array_no_zero(scratch.arena, U8, file_size);
    DWORD bytes_read = 0;
    read_ok = ReadFile(file, buf, (DWORD)file_size, &bytes_read, 0) && bytes_read == file_size;
  }
  CloseHandle(file);

  if(read_ok && file_size > 0)
  {
    result = kh_store__deserialize(arena, buf, file_size);
  }

  scratch_end(scratch);
  return result;
}

internal void
kh_store_save(KH_HostArray *hosts)
{
  Temp scratch = scratch_begin(0, 0);

  WCHAR *path = 0;
  if(!kh_store__get_path(scratch.arena, &path))
  {
    scratch_end(scratch);
    return;
  }

  U64 size = 0;
  U8 *buf = kh_store__serialize(scratch.arena, hosts, &size);

  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if(file != INVALID_HANDLE_VALUE)
  {
    DWORD bytes_written = 0;
    WriteFile(file, buf, (DWORD)size, &bytes_written, 0);
    CloseHandle(file);
  }

  scratch_end(scratch);
}

internal B32
kh_store_find(KH_HostArray *hosts, String8 host, U16 port, U8 out_hash[KH_HASH_SIZE])
{
  for(U64 i = 0; i < hosts->count; i += 1)
  {
    if(hosts->v[i].port == port && str8_match(hosts->v[i].host, host))
    {
      MemoryCopy(out_hash, hosts->v[i].hash, KH_HASH_SIZE);
      return 1;
    }
  }
  return 0;
}

internal void
kh_store_set(KH_HostArray *hosts, Arena *arena, String8 host, U16 port, U8 hash[KH_HASH_SIZE])
{
  for(U64 i = 0; i < hosts->count; i += 1)
  {
    if(hosts->v[i].port == port && str8_match(hosts->v[i].host, host))
    {
      MemoryCopy(hosts->v[i].hash, hash, KH_HASH_SIZE);
      return;
    }
  }
  if(hosts->count < KH_MAX_HOSTS)
  {
    KH_Host *h = &hosts->v[hosts->count];
    h->host = str8_copy(arena, host);
    h->port = port;
    MemoryCopy(h->hash, hash, KH_HASH_SIZE);
    hosts->count += 1;
  }
}
