// Threading & synchronization primitives. Mirrors raddebugger's
// src/base/base_threads.h almost exactly (Thread, Mutex, RWMutex, CondVar,
// Semaphore, the MutexScope/RWMutexScope defer-loop macros) - trimmed to
// drop Stripe/Barrier machinery this project doesn't need yet.
//
// These are opaque handles (a single U64) so the OS-specific storage behind
// them can live in a small entity pool (see os_core_win32.c) rather than
// forcing every caller to malloc a platform object.

#ifndef BASE_THREADS_H
#define BASE_THREADS_H

////////////////////////////////
//~ Thread Types

typedef struct Thread Thread;
struct Thread
{
  U64 u64[1];
};
typedef void ThreadEntryPointFunctionType(void *p);

////////////////////////////////
//~ Synchronization Primitive Types

typedef struct Mutex Mutex;
struct Mutex
{
  U64 u64[1];
};

typedef struct RWMutex RWMutex;
struct RWMutex
{
  U64 u64[1];
};

typedef struct CondVar CondVar;
struct CondVar
{
  U64 u64[1];
};

typedef struct Semaphore Semaphore;
struct Semaphore
{
  U64 u64[1];
};

////////////////////////////////
//~ Thread Info Helpers

internal void set_thread_name(String8 string);
internal void set_thread_namef(char *fmt, ...);

////////////////////////////////
//~ @per_os_impl Current Thread / Time Info

internal U32  tid(void);
internal void set_platform_thread_name(String8 name);
internal U64  now_time_us(void);
internal void sleep_ms(U32 ms);

////////////////////////////////
//~ @per_os_impl Thread Functions

internal Thread thread_launch(ThreadEntryPointFunctionType *f, void *p);
internal B32    thread_join(Thread thread, U64 endt_us);
internal void   thread_detach(Thread thread);

////////////////////////////////
//~ @per_os_impl Synchronization Primitive Functions

//- recursive mutexes
internal Mutex mutex_alloc(void);
internal void  mutex_release(Mutex mutex);
internal void  mutex_take(Mutex mutex);
internal void  mutex_drop(Mutex mutex);

//- reader/writer mutexes
internal RWMutex rw_mutex_alloc(void);
internal void    rw_mutex_release(RWMutex mutex);
internal void    rw_mutex_take(RWMutex mutex, B32 write_mode);
internal void    rw_mutex_drop(RWMutex mutex, B32 write_mode);
#define rw_mutex_take_r(m) rw_mutex_take((m), (0))
#define rw_mutex_take_w(m) rw_mutex_take((m), (1))
#define rw_mutex_drop_r(m) rw_mutex_drop((m), (0))
#define rw_mutex_drop_w(m) rw_mutex_drop((m), (1))

//- condition variables
internal CondVar cond_var_alloc(void);
internal void    cond_var_release(CondVar cv);
// returns false on timeout, true on signal. endt_us = max_U64 -> no timeout
internal B32     cond_var_wait(CondVar cv, Mutex mutex, U64 endt_us);
internal void    cond_var_signal(CondVar cv);
internal void    cond_var_broadcast(CondVar cv);

//- semaphores
internal Semaphore semaphore_alloc(U32 initial_count, U32 max_count);
internal void      semaphore_release(Semaphore semaphore);
internal B32       semaphore_take(Semaphore semaphore, U64 endt_us);
internal void      semaphore_drop(Semaphore semaphore);
internal void      semaphore_drop_count(Semaphore semaphore, U64 drop_count);

//- scope macros: no RAII, just a for-loop that runs its body exactly once
#define MutexScope(mutex)             DeferLoop(mutex_take(mutex), mutex_drop(mutex))
#define RWMutexScope(mutex, write_mode) DeferLoop(rw_mutex_take((mutex), (write_mode)), rw_mutex_drop((mutex), (write_mode)))
#define MutexScopeR(mutex) DeferLoop(rw_mutex_take_r(mutex), rw_mutex_drop_r(mutex))
#define MutexScopeW(mutex) DeferLoop(rw_mutex_take_w(mutex), rw_mutex_drop_w(mutex))

#endif // BASE_THREADS_H
