internal Arena *
arena_alloc_(ArenaParams *params)
{
  U64 reserve_size = AlignPow2(params->reserve_size, get_system_info()->page_size);
  U64 commit_size  = AlignPow2(params->commit_size,  get_system_info()->page_size);

  void *base = reserve_memory(reserve_size);
  if(Unlikely(base == 0))
  {
    abort_self(1);
  }
  commit_memory(base, commit_size);

  Arena *arena    = (Arena *)base;
  arena->prev     = 0;
  arena->current  = arena;
  arena->cmt_size = params->commit_size;
  arena->res_size = params->reserve_size;
  arena->base_pos = 0;
  arena->pos      = ARENA_HEADER_SIZE;
  arena->cmt      = commit_size;
  arena->res      = reserve_size;
  return arena;
}

internal void
arena_release(Arena *arena)
{
  for(Arena *n = arena->current, *prev = 0; n != 0; n = prev)
  {
    prev = n->prev;
    release_memory(n, n->res);
  }
}

internal void *
arena_push(Arena *arena, U64 size, U64 align)
{
  Arena *current = arena->current;
  U64 pos_pre = AlignPow2(current->pos, align);
  // An oversized/corrupt `size` (should never happen from well-behaved
  // call sites, which clamp against a real destination first - but this
  // is the one central allocation choke point every layer routes
  // through) wrapping pos_pre+size back down to a small value would make
  // the checks below think a tiny block is big enough, handing the
  // caller a pointer to a real overflow the moment it writes `size`
  // bytes. Treated as unrecoverable API misuse, same as any other
  // can't-happen case in this codebase.
  AssertAlways(size <= max_U64 - pos_pre);
  U64 pos_pst = pos_pre + size;

  // rjf-style: chain to a new block if this one can't fit the request
  if(current->res < pos_pst)
  {
    U64 res_size = arena->res_size;
    U64 cmt_size = arena->cmt_size;
    if(size + ARENA_HEADER_SIZE > res_size)
    {
      res_size = AlignPow2(size + ARENA_HEADER_SIZE, align);
      cmt_size = AlignPow2(size + ARENA_HEADER_SIZE, align);
    }
    Arena *new_block = arena_alloc(.reserve_size = res_size, .commit_size = cmt_size);
    new_block->base_pos = current->base_pos + current->res;
    SLLStackPush_N(arena->current, new_block, prev);
    current = new_block;
    pos_pre = AlignPow2(current->pos, align);
    pos_pst = pos_pre + size;
  }

  // commit new pages, if needed
  if(current->cmt < pos_pst)
  {
    U64 cmt_pst_aligned = AlignPow2(pos_pst, current->cmt_size);
    U64 cmt_pst_clamped = ClampTop(cmt_pst_aligned, current->res);
    U64 cmt_size = cmt_pst_clamped - current->cmt;
    U8 *cmt_ptr = (U8 *)current + current->cmt;
    commit_memory(cmt_ptr, cmt_size);
    current->cmt = cmt_pst_clamped;
  }

  void *result = 0;
  if(current->cmt >= pos_pst)
  {
    result = (U8 *)current + pos_pre;
    current->pos = pos_pst;
  }
  if(Unlikely(result == 0))
  {
    abort_self(1);
  }
  return result;
}

internal U64
arena_pos(Arena *arena)
{
  Arena *current = arena->current;
  return current->base_pos + current->pos;
}

internal void
arena_pop_to(Arena *arena, U64 pos)
{
  U64 big_pos = ClampBot(ARENA_HEADER_SIZE, pos);
  Arena *current = arena->current;
  for(Arena *prev = 0; current->base_pos >= big_pos && current->prev != 0; current = prev)
  {
    prev = current->prev;
    release_memory(current, current->res);
  }
  arena->current = current;
  U64 new_pos = big_pos - current->base_pos;
  AssertAlways(new_pos <= current->pos);
  current->pos = new_pos;
}

internal void
arena_clear(Arena *arena)
{
  arena_pop_to(arena, 0);
}

internal void
arena_pop(Arena *arena, U64 amt)
{
  U64 pos_old = arena_pos(arena);
  U64 pos_new = pos_old;
  if(amt < pos_old)
  {
    pos_new = pos_old - amt;
  }
  arena_pop_to(arena, pos_new);
}

internal Temp
temp_begin(Arena *arena)
{
  U64 pos = arena_pos(arena);
  Temp temp = {arena, pos};
  return temp;
}

internal void
temp_end(Temp temp)
{
  arena_pop_to(temp.arena, temp.pos);
}
