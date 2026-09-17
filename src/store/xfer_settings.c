// Builds "%APPDATA%\lwftpclient\settings.dat" as a null-terminated wide
// string, creating the containing directory along the way (independently
// of conn_store.c's own directory-creation - don't assume it's already
// there, settings could be saved before any connection profile is).
internal B32
xfer_settings__get_path(Arena *arena, WCHAR **out_path)
{
  char *appdata = getenv("APPDATA");
  if(appdata == 0)
  {
    return 0;
  }

  Temp scratch = scratch_begin(&arena, 1);
  String8 appdata_s8 = str8_cstring(appdata);
  String8 dir_s8 = str8f(scratch.arena, "%.*s\\lwftpclient", str8_varg(appdata_s8));
  String8 file_s8 = str8f(scratch.arena, "%.*s\\settings.dat", str8_varg(dir_s8));

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

internal U32
xfer_settings__clamp(U32 v)
{
  if(v < 1)
  {
    v = 1;
  }
  if(v > XFER_SETTINGS_HARD_CAP)
  {
    v = XFER_SETTINGS_HARD_CAP;
  }
  return v;
}

internal XFER_Settings
xfer_settings_load(void)
{
  XFER_Settings result = {2, 2};
  Temp scratch = scratch_begin(0, 0);

  WCHAR *path = 0;
  if(!xfer_settings__get_path(scratch.arena, &path))
  {
    scratch_end(scratch);
    return result;
  }

  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if(file != INVALID_HANDLE_VALUE)
  {
    U32 buf[2] = {0};
    DWORD bytes_read = 0;
    if(ReadFile(file, buf, sizeof(buf), &bytes_read, 0) && bytes_read == sizeof(buf))
    {
      result.max_downloads = xfer_settings__clamp(buf[0]);
      result.max_uploads = xfer_settings__clamp(buf[1]);
    }
    CloseHandle(file);
  }

  scratch_end(scratch);
  return result;
}

internal void
xfer_settings_save(XFER_Settings *settings)
{
  Temp scratch = scratch_begin(0, 0);

  WCHAR *path = 0;
  if(!xfer_settings__get_path(scratch.arena, &path))
  {
    scratch_end(scratch);
    return;
  }

  U32 buf[2] = { xfer_settings__clamp(settings->max_downloads), xfer_settings__clamp(settings->max_uploads) };
  HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if(file != INVALID_HANDLE_VALUE)
  {
    DWORD bytes_written = 0;
    WriteFile(file, buf, sizeof(buf), &bytes_written, 0);
    CloseHandle(file);
  }

  scratch_end(scratch);
}
