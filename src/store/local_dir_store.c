// Builds "%APPDATA%\FTP-FTW\local_dir.dat" - same pattern as
// xfer_settings__get_path/conn_store__get_path (one copy per store rather
// than a shared helper - three call sites doesn't earn an abstraction yet).
internal B32
local_dir_store__get_path(Arena *arena, WCHAR **out_path)
{
  char *appdata = getenv("APPDATA");
  if(appdata == 0)
  {
    return 0;
  }

  Temp scratch = scratch_begin(&arena, 1);
  String8 appdata_s8 = str8_cstring(appdata);
  String8 dir_s8 = str8f(scratch.arena, "%.*s\\FTP-FTW", str8_varg(appdata_s8));
  String8 file_s8 = str8f(scratch.arena, "%.*s\\local_dir.dat", str8_varg(dir_s8));

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

internal String8
local_dir_store_load(Arena *arena)
{
  String8 result = {0};
  Temp scratch = scratch_begin(&arena, 1);

  WCHAR *path = 0;
  if(!local_dir_store__get_path(scratch.arena, &path))
  {
    scratch_end(scratch);
    return result;
  }

  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if(file != INVALID_HANDLE_VALUE)
  {
    LARGE_INTEGER size_li = {0};
    GetFileSizeEx(file, &size_li);
    U64 file_size = (U64)size_li.QuadPart;
    if(file_size > 0 && file_size <= LOCAL_DIR_STORE_MAX_SIZE)
    {
      U8 *buf = push_array_no_zero(arena, U8, file_size);
      DWORD bytes_read = 0;
      if(ReadFile(file, buf, (DWORD)file_size, &bytes_read, 0) && bytes_read == file_size)
      {
        result = str8(buf, file_size);
      }
    }
    CloseHandle(file);
  }

  scratch_end(scratch);
  return result;
}

internal void
local_dir_store_save(String8 path)
{
  Temp scratch = scratch_begin(0, 0);

  WCHAR *file_path = 0;
  if(!local_dir_store__get_path(scratch.arena, &file_path))
  {
    scratch_end(scratch);
    return;
  }

  U64 write_size = Min(path.size, (U64)LOCAL_DIR_STORE_MAX_SIZE);
  HANDLE file = CreateFileW(file_path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if(file != INVALID_HANDLE_VALUE)
  {
    DWORD bytes_written = 0;
    WriteFile(file, path.str, (DWORD)write_size, &bytes_written, 0);
    CloseHandle(file);
  }

  scratch_end(scratch);
}
