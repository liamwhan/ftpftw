// "Ryan's strings": String8 is a { U8 *str; U64 size; } pair, NOT
// null-terminated. All path handling, FTP/SFTP text, and UI text in this
// codebase goes through String8. Modeled on raddebugger's
// src/base/base_strings.h, trimmed to what this project needs so far.

#ifndef BASE_STRINGS_H
#define BASE_STRINGS_H

////////////////////////////////
//~ String Types

typedef struct String8 String8;
struct String8
{
  U8 *str;
  U64 size;
};

typedef struct String8Node String8Node;
struct String8Node
{
  String8Node *next;
  String8 string;
};

typedef struct String8List String8List;
struct String8List
{
  String8Node *first;
  String8Node *last;
  U64 node_count;
  U64 total_size;
};

////////////////////////////////
//~ Constructors

#define str8_varg(s) (int)(s).size, (s).str
internal String8 str8(U8 *str, U64 size);
internal String8 str8_cstring(char *cstr);
#define str8_lit(s) str8((U8 *)(s), sizeof(s) - 1)

////////////////////////////////
//~ Matching

internal B32 str8_match(String8 a, String8 b);

////////////////////////////////
//~ Allocation

internal String8 str8_copy(Arena *arena, String8 s);
internal String8 str8_cstring_copy(Arena *arena, char *cstr);
internal char   *str8_to_cstring(Arena *arena, String8 s);
internal String8 str8fv(Arena *arena, char *fmt, va_list args);
internal String8 str8f(Arena *arena, char *fmt, ...);

////////////////////////////////
//~ String Lists

internal void    str8_list_push(Arena *arena, String8List *list, String8 string);
internal void    str8_list_pushf(Arena *arena, String8List *list, char *fmt, ...);
internal String8 str8_list_join(Arena *arena, String8List *list, String8 sep);

#endif // BASE_STRINGS_H
