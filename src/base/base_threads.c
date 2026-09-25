internal void
set_thread_name(String8 string)
{
  set_platform_thread_name(string);
}

internal void
set_thread_namef(char *fmt, ...)
{
  Temp scratch = scratch_begin(0, 0);
  va_list args;
  va_start(args, fmt);
  String8 string = str8fv(scratch.arena, fmt, args);
  va_end(args);
  set_thread_name(string);
  scratch_end(scratch);
}

internal void
semaphore_drop(Semaphore semaphore)
{
  semaphore_drop_count(semaphore, 1);
}
