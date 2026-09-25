// Arena allocator: reserve/commit virtual memory up front, bump-allocate
// linearly out of it, chain to a new block on overflow. No malloc/free per
// object anywhere in this codebase - this is the memory model. Modeled
// directly on raddebugger's src/base/base_arena.h.

#ifndef BASE_ARENA_H
#define BASE_ARENA_H

#define ARENA_HEADER_SIZE 128

typedef struct ArenaParams ArenaParams;
struct ArenaParams
{
  U64 reserve_size;
  U64 commit_size;
};

typedef struct Arena Arena;
struct Arena
{
  Arena *prev;    // previous arena in chain
  Arena *current; // current arena in chain
  U64 cmt_size;
  U64 res_size;
  U64 base_pos;
  U64 pos;
  U64 cmt;
  U64 res;
};
StaticAssert(sizeof(Arena) <= ARENA_HEADER_SIZE, arena_header_size_check);

typedef struct Temp Temp;
struct Temp
{
  Arena *arena;
  U64 pos;
};

global U64 arena_default_reserve_size = MB(64);
global U64 arena_default_commit_size  = KB(64);

//- arena creation/destruction
internal Arena *arena_alloc_(ArenaParams *params);
#define arena_alloc(...) arena_alloc_(&(ArenaParams){.reserve_size = arena_default_reserve_size, .commit_size = arena_default_commit_size, __VA_ARGS__})
internal void arena_release(Arena *arena);

//- arena push/pop/pos core functions
internal void *arena_push(Arena *arena, U64 size, U64 align);
internal U64   arena_pos(Arena *arena);
internal void  arena_pop_to(Arena *arena, U64 pos);

//- arena push/pop helpers
internal void arena_clear(Arena *arena);
internal void arena_pop(Arena *arena, U64 amt);

//- temporary arena scopes
internal Temp temp_begin(Arena *arena);
internal void temp_end(Temp temp);

//- push helper macros
#define push_array_no_zero(arena, T, count) (T *)arena_push((arena), sizeof(T) * (count), Max(8, _Alignof(T)))
#define push_array(arena, T, count) (T *)MemoryZero(push_array_no_zero(arena, T, count), sizeof(T) * (count))

#endif // BASE_ARENA_H
