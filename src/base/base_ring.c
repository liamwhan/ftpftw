internal U64
ring__wrapped_write(U8 *base, U64 size, U64 pos, void *src, U64 src_size)
{
  U64 pos_mod = pos % size;
  U64 first_copy_size = Min(src_size, size - pos_mod);
  U64 second_copy_size = src_size - first_copy_size;
  MemoryCopy(base + pos_mod, src, first_copy_size);
  MemoryCopy(base, (U8 *)src + first_copy_size, second_copy_size);
  return src_size;
}

internal U64
ring__wrapped_read(U8 *base, U64 size, U64 pos, void *dst, U64 dst_size)
{
  U64 pos_mod = pos % size;
  U64 first_copy_size = Min(dst_size, size - pos_mod);
  U64 second_copy_size = dst_size - first_copy_size;
  MemoryCopy(dst, base + pos_mod, first_copy_size);
  MemoryCopy((U8 *)dst + first_copy_size, base, second_copy_size);
  return dst_size;
}

internal Ring
make_ring(Arena *arena, U64 size)
{
  Ring ring = {0};
  ring.size = size;
  ring.base = push_array_no_zero(arena, U8, size);
  return ring;
}

internal B32
ring_try_write(Ring *ring, U64 size, void *ptr)
{
  U64 bytes_unconsumed = ring->write_pos - ring->read_pos;
  U64 bytes_available = ring->size - bytes_unconsumed;
  B32 result = 0;
  if(bytes_available >= size)
  {
    result = 1;
    ring->write_pos += ring__wrapped_write(ring->base, ring->size, ring->write_pos, ptr, size);
  }
  return result;
}

internal B32
ring_try_read(Ring *ring, U64 size, void *ptr)
{
  U64 bytes_unconsumed = ring->write_pos - ring->read_pos;
  B32 result = 0;
  if(bytes_unconsumed >= size)
  {
    result = 1;
    ring->read_pos += ring__wrapped_read(ring->base, ring->size, ring->read_pos, ptr, size);
  }
  return result;
}

internal GuardedRing
guarded_ring_alloc(Arena *arena, U64 size)
{
  GuardedRing gr = {0};
  gr.ring = make_ring(arena, size);
  gr.mutex = mutex_alloc();
  gr.cv = cond_var_alloc();
  return gr;
}

internal void
guarded_ring_release(GuardedRing *ring)
{
  mutex_release(ring->mutex);
  cond_var_release(ring->cv);
}

internal B32
guarded_ring_write_or_wait(GuardedRing *ring, U64 size, void *ptr, U64 endt_us)
{
  B32 write_good = 0;
  MutexScope(ring->mutex)
  {
    for(;;)
    {
      write_good = ring_try_write(&ring->ring, size, ptr);
      if(write_good)
      {
        cond_var_broadcast(ring->cv);
        break;
      }
      if(now_time_us() >= endt_us || !cond_var_wait(ring->cv, ring->mutex, endt_us))
      {
        break;
      }
    }
  }
  return write_good;
}

internal B32
guarded_ring_read_or_wait(GuardedRing *ring, U64 size, void *ptr, U64 endt_us)
{
  B32 read_good = 0;
  MutexScope(ring->mutex)
  {
    for(;;)
    {
      read_good = ring_try_read(&ring->ring, size, ptr);
      if(read_good)
      {
        cond_var_broadcast(ring->cv);
        break;
      }
      if(now_time_us() >= endt_us || !cond_var_wait(ring->cv, ring->mutex, endt_us))
      {
        break;
      }
    }
  }
  return read_good;
}
