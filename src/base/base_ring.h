// Ring / GuardedRing: the cross-thread message-passing primitive. The
// console/UI thread talks to the control-connection thread, and the control
// thread talks to transfer threads, by writing fixed-size structs into a
// GuardedRing and reading them back out - never by reaching into another
// thread's state directly. Mirrors raddebugger's src/base/base_ring.h.

#ifndef BASE_RING_H
#define BASE_RING_H

typedef struct Ring Ring;
struct Ring
{
  U8 *base;
  U64 size;
  U64 write_pos;
  U64 read_pos;
};

typedef struct GuardedRing GuardedRing;
struct GuardedRing
{
  Ring ring;
  Mutex mutex;
  CondVar cv;
};

internal Ring make_ring(Arena *arena, U64 size);
internal B32  ring_try_write(Ring *ring, U64 size, void *ptr);
internal B32  ring_try_read(Ring *ring, U64 size, void *ptr);
#define ring_try_write_struct(ring, ptr) ring_try_write((ring), sizeof(*(ptr)), (ptr))
#define ring_try_read_struct(ring, ptr) ring_try_read((ring), sizeof(*(ptr)), (ptr))

internal GuardedRing guarded_ring_alloc(Arena *arena, U64 size);
internal void guarded_ring_release(GuardedRing *ring);
internal B32  guarded_ring_write_or_wait(GuardedRing *ring, U64 size, void *ptr, U64 endt_us);
internal B32  guarded_ring_read_or_wait(GuardedRing *ring, U64 size, void *ptr, U64 endt_us);
#define guarded_ring_write_struct_or_wait(ring, ptr, endt_us) guarded_ring_write_or_wait((ring), sizeof(*(ptr)), (ptr), (endt_us))
#define guarded_ring_read_struct_or_wait(ring, ptr, endt_us) guarded_ring_read_or_wait((ring), sizeof(*(ptr)), (ptr), (endt_us))

#endif // BASE_RING_H
