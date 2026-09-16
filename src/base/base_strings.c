internal String8
str8(U8 *str, U64 size)
{
  String8 result = {str, size};
  return result;
}

internal String8
str8_cstring(char *cstr)
{
  String8 result = {(U8 *)cstr, cstr ? strlen(cstr) : 0};
  return result;
}

internal B32
str8_match(String8 a, String8 b)
{
  return a.size == b.size && MemoryMatch(a.str, b.str, a.size);
}

internal String8
str8_copy(Arena *arena, String8 s)
{
  String8 result;
  result.str = push_array_no_zero(arena, U8, s.size + 1);
  result.size = s.size;
  MemoryCopy(result.str, s.str, s.size);
  result.str[s.size] = 0;
  return result;
}

internal String8
str8_cstring_copy(Arena *arena, char *cstr)
{
  return str8_copy(arena, str8_cstring(cstr));
}

internal char *
str8_to_cstring(Arena *arena, String8 s)
{
  char *result = push_array_no_zero(arena, char, s.size + 1);
  MemoryCopy(result, s.str, s.size);
  result[s.size] = 0;
  return result;
}

internal String8
str8fv(Arena *arena, char *fmt, va_list args)
{
  va_list args2;
  va_copy(args2, args);
  U64 needed = (U64)vsnprintf(0, 0, fmt, args) + 1;
  U8 *buffer = push_array_no_zero(arena, U8, needed);
  U64 actual = (U64)vsnprintf((char *)buffer, needed, fmt, args2);
  va_end(args2);
  String8 result = {buffer, actual};
  return result;
}

internal String8
str8f(Arena *arena, char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  String8 result = str8fv(arena, fmt, args);
  va_end(args);
  return result;
}

////////////////////////////////
//~ String Lists

internal void
str8_list_push(Arena *arena, String8List *list, String8 string)
{
  String8Node *node = push_array(arena, String8Node, 1);
  node->string = string;
  SLLQueuePush(list->first, list->last, node);
  list->node_count += 1;
  list->total_size += string.size;
}

internal void
str8_list_pushf(Arena *arena, String8List *list, char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  String8 string = str8fv(arena, fmt, args);
  va_end(args);
  str8_list_push(arena, list, string);
}

internal String8
str8_list_join(Arena *arena, String8List *list, String8 sep)
{
  U64 total_size = list->total_size + sep.size * (list->node_count > 0 ? list->node_count - 1 : 0);
  U8 *buffer = push_array_no_zero(arena, U8, total_size + 1);
  U64 cursor = 0;
  for(String8Node *n = list->first; n != 0; n = n->next)
  {
    if(n != list->first && sep.size != 0)
    {
      MemoryCopy(buffer + cursor, sep.str, sep.size);
      cursor += sep.size;
    }
    MemoryCopy(buffer + cursor, n->string.str, n->string.size);
    cursor += n->string.size;
  }
  buffer[cursor] = 0;
  String8 result = {buffer, cursor};
  return result;
}
