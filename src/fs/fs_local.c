internal int
fs_entry_compare(const void *a, const void *b)
{
  const FS_Entry *ea = (const FS_Entry *)a;
  const FS_Entry *eb = (const FS_Entry *)b;
  if(ea->is_dir != eb->is_dir)
  {
    return ea->is_dir ? -1 : 1; // directories first
  }
  U64 min_size = Min(ea->name.size, eb->name.size);
  int cmp = MemoryCompare(ea->name.str, eb->name.str, min_size);
  if(cmp == 0 && ea->name.size != eb->name.size)
  {
    cmp = (ea->name.size < eb->name.size) ? -1 : 1;
  }
  return cmp;
}

internal B32
fs_path_is_root(String8 path)
{
  return path.size == 3 && path.str[1] == ':' && path.str[2] == '\\';
}

internal String8
fs_path_join(Arena *arena, String8 dir, String8 name)
{
  B32 needs_sep = (dir.size > 0 && dir.str[dir.size - 1] != '\\');
  return str8f(arena, "%.*s%s%.*s",
               (int)dir.size, (char *)dir.str,
               needs_sep ? "\\" : "",
               (int)name.size, (char *)name.str);
}

internal String8
fs_path_parent(Arena *arena, String8 path)
{
  String8 trimmed = path;
  if(trimmed.size > 0 && trimmed.str[trimmed.size - 1] == '\\')
  {
    trimmed.size -= 1;
  }

  S64 slash_idx = -1;
  for(S64 i = (S64)trimmed.size - 1; i >= 0; i -= 1)
  {
    if(trimmed.str[i] == '\\')
    {
      slash_idx = i;
      break;
    }
  }

  String8 result;
  if(slash_idx < 2)
  {
    result = str8_copy(arena, path); // malformed/no parent - stay put
  }
  else if(slash_idx == 2)
  {
    result = str8_copy(arena, str8(trimmed.str, 3)); // "C:\Foo" -> "C:\"
  }
  else
  {
    result = str8_copy(arena, str8(trimmed.str, (U64)slash_idx));
  }
  return result;
}

internal FS_EntryArray
fs_list_dir(Arena *arena, String8 path)
{
  local_persist FS_Entry temp_entries[1024];
  U64 count = 0;

  Temp scratch = scratch_begin(&arena, 1);

  B32 ends_in_sep = (path.size > 0 && path.str[path.size - 1] == '\\');
  String8 pattern = str8f(scratch.arena, "%.*s%s*", (int)path.size, (char *)path.str, ends_in_sep ? "" : "\\");

  int wide_count = MultiByteToWideChar(CP_UTF8, 0, (char *)pattern.str, (int)pattern.size, 0, 0);
  WCHAR *wide_pattern = push_array_no_zero(scratch.arena, WCHAR, wide_count + 1);
  MultiByteToWideChar(CP_UTF8, 0, (char *)pattern.str, (int)pattern.size, wide_pattern, wide_count);
  wide_pattern[wide_count] = 0;

  B32 is_root = fs_path_is_root(path);
  WIN32_FIND_DATAW find_data;
  HANDLE h = FindFirstFileW(wide_pattern, &find_data);
  if(h != INVALID_HANDLE_VALUE)
  {
    do
    {
      B32 is_dot = (find_data.cFileName[0] == L'.' && find_data.cFileName[1] == 0);
      B32 is_dotdot = (find_data.cFileName[0] == L'.' && find_data.cFileName[1] == L'.' && find_data.cFileName[2] == 0);
      B32 skip = is_dot || (is_dotdot && is_root);
      if(!skip && count < ArrayCount(temp_entries))
      {
        int utf8_count = WideCharToMultiByte(CP_UTF8, 0, find_data.cFileName, -1, 0, 0, 0, 0);
        U8 *utf8_name = push_array_no_zero(arena, U8, utf8_count);
        WideCharToMultiByte(CP_UTF8, 0, find_data.cFileName, -1, (char *)utf8_name, utf8_count, 0, 0);
        U64 name_len = (utf8_count > 0) ? (U64)(utf8_count - 1) : 0; // drop the null WideCharToMultiByte wrote

        FS_Entry *e = &temp_entries[count];
        e->name = str8(utf8_name, name_len);
        e->is_dir = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->size = ((U64)find_data.nFileSizeHigh << 32) | find_data.nFileSizeLow;
        count += 1;
      }
    }
    while(FindNextFileW(h, &find_data));
    FindClose(h);
  }

  qsort(temp_entries, count, sizeof(FS_Entry), fs_entry_compare);

  FS_EntryArray result = {0};
  result.count = count;
  result.v = push_array_no_zero(arena, FS_Entry, count);
  MemoryCopy(result.v, temp_entries, sizeof(FS_Entry) * count);

  scratch_end(scratch);
  return result;
}

internal WCHAR *
fs__wide_from_path(Arena *arena, String8 path)
{
  int wide_count = MultiByteToWideChar(CP_UTF8, 0, (char *)path.str, (int)path.size, 0, 0);
  WCHAR *wide = push_array_no_zero(arena, WCHAR, wide_count + 1);
  MultiByteToWideChar(CP_UTF8, 0, (char *)path.str, (int)path.size, wide, wide_count);
  wide[wide_count] = 0;
  return wide;
}

internal FS_File
fs_file_open_read(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  WCHAR *wide = fs__wide_from_path(scratch.arena, path);
  HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  scratch_end(scratch);
  FS_File result = {(U64)h};
  return result;
}

internal FS_File
fs_file_open_write(String8 path)
{
  Temp scratch = scratch_begin(0, 0);
  WCHAR *wide = fs__wide_from_path(scratch.arena, path);
  HANDLE h = CreateFileW(wide, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  scratch_end(scratch);
  FS_File result = {(U64)h};
  return result;
}

internal B32
fs_file_is_valid(FS_File file)
{
  HANDLE h = (HANDLE)PtrFromInt(file.u64[0]);
  return h != 0 && h != INVALID_HANDLE_VALUE;
}

internal U64
fs_file_read(FS_File file, void *buf, U64 max_size)
{
  HANDLE h = (HANDLE)PtrFromInt(file.u64[0]);
  DWORD read_bytes = 0;
  BOOL ok = ReadFile(h, buf, (DWORD)max_size, &read_bytes, 0);
  return ok ? (U64)read_bytes : 0;
}

internal B32
fs_file_write(FS_File file, void *buf, U64 size)
{
  HANDLE h = (HANDLE)PtrFromInt(file.u64[0]);
  U8 *p = (U8 *)buf;
  U64 remaining = size;
  while(remaining > 0)
  {
    DWORD written = 0;
    BOOL ok = WriteFile(h, p, (DWORD)remaining, &written, 0);
    if(!ok || written == 0)
    {
      return 0;
    }
    p += written;
    remaining -= written;
  }
  return 1;
}

internal void
fs_file_close(FS_File file)
{
  HANDLE h = (HANDLE)PtrFromInt(file.u64[0]);
  if(h != 0 && h != INVALID_HANDLE_VALUE)
  {
    CloseHandle(h);
  }
}

internal B32
fs_file_size(String8 path, U64 *out_size)
{
  Temp scratch = scratch_begin(0, 0);
  WCHAR *wide = fs__wide_from_path(scratch.arena, path);
  WIN32_FILE_ATTRIBUTE_DATA data = {0};
  BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &data);
  scratch_end(scratch);
  if(ok)
  {
    *out_size = ((U64)data.nFileSizeHigh << 32) | data.nFileSizeLow;
  }
  return ok != 0;
}
