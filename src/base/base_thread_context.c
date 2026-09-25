thread_static TCTX *tctx_thread_local = 0;

internal TCTX *
tctx_alloc(void)
{
  Arena *arena0 = arena_alloc();
  TCTX *tctx = push_array(arena0, TCTX, 1);
  tctx->arenas[0] = arena0;
  tctx->arenas[1] = arena_alloc();
  return tctx;
}

internal void
tctx_release(TCTX *tctx)
{
  Arena *arena1 = tctx->arenas[1];
  Arena *arena0 = tctx->arenas[0];
  arena_release(arena1);
  arena_release(arena0);
}

internal void
tctx_select(TCTX *tctx)
{
  tctx_thread_local = tctx;
}

internal TCTX *
tctx_selected(void)
{
  return tctx_thread_local;
}

internal Arena *
tctx_get_scratch(Arena **conflicts, U64 count)
{
  TCTX *tctx = tctx_selected();
  Arena *result = 0;
  for(U64 slot = 0; slot < ArrayCount(tctx->arenas); slot += 1)
  {
    Arena *arena = tctx->arenas[slot];
    B32 conflicts_with_caller = 0;
    for(U64 i = 0; i < count; i += 1)
    {
      if(conflicts[i] == arena)
      {
        conflicts_with_caller = 1;
        break;
      }
    }
    if(!conflicts_with_caller)
    {
      result = arena;
      break;
    }
  }
  return result;
}
