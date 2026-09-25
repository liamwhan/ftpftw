// TCTX: per-thread context bundle. Every thread this codebase spawns
// (control-connection thread, each transfer thread, the console/main
// thread) gets one of these, so scratch allocations on one thread never
// contend with another thread's scratch space. Mirrors raddebugger's
// src/base/base_thread_context.h, trimmed to scratch arenas + progress
// counters (no lane/access-scope machinery - not needed yet).

#ifndef BASE_THREAD_CONTEXT_H
#define BASE_THREAD_CONTEXT_H

typedef struct TCTX TCTX;
struct TCTX
{
  Arena *arenas[2];
  U8 thread_name[32];
  U64 thread_name_size;
  U64 *progress_counter_ptr;
  U64 *progress_target_ptr;
};

extern thread_static TCTX *tctx_thread_local;

//- thread-context allocation & selection
internal TCTX *tctx_alloc(void);
internal void  tctx_release(TCTX *tctx);
internal void  tctx_select(TCTX *tctx);
internal TCTX *tctx_selected(void);

//- scratch arenas
internal Arena *tctx_get_scratch(Arena **conflicts, U64 count);
#define scratch_begin(conflicts, count) temp_begin(tctx_get_scratch((conflicts), (count)))
#define scratch_end(scratch) temp_end(scratch)

//- progress counters: lets a transfer thread report bytes-transferred
//  without whatever's watching (console today, GUI later) polling
//  connection state directly.
#define set_progress_ptr(ptr)        (tctx_selected()->progress_counter_ptr = (ptr))
#define set_progress_target_ptr(ptr) (tctx_selected()->progress_target_ptr = (ptr))
#define set_progress(val)  (tctx_selected()->progress_counter_ptr ? ins_atomic_u64_eval_assign(tctx_selected()->progress_counter_ptr, (val)) : (void)0)
#define add_progress(val)  (tctx_selected()->progress_counter_ptr ? ins_atomic_u64_add_eval(tctx_selected()->progress_counter_ptr, (val)) : (void)0)
#define set_progress_target(val) (tctx_selected()->progress_target_ptr ? ins_atomic_u64_eval_assign(tctx_selected()->progress_target_ptr, (val)) : (void)0)

#endif // BASE_THREAD_CONTEXT_H
