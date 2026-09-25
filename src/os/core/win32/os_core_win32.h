// Windows implementation of the OS-facing pieces the base layer declares as
// @per_os_impl: system info, virtual memory, threads, and synchronization
// primitives. Mirrors raddebugger's src/win32/base/win32_base.h entity-pool
// trick: Thread/Mutex/RWMutex/CondVar are opaque U64 handles pointing into a
// free-listed pool of W32_Entity, so callers never see a raw Win32 type and
// repeated alloc/release (e.g. one thread per transfer) doesn't leak entities
// or hit the heap.

#ifndef OS_CORE_WIN32_H
#define OS_CORE_WIN32_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <processthreadsapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "kernel32")
#pragma comment(lib, "ws2_32")

typedef enum W32_EntityKind
{
  W32_EntityKind_Null,
  W32_EntityKind_Thread,
  W32_EntityKind_Mutex,
  W32_EntityKind_RWMutex,
  W32_EntityKind_ConditionVariable,
}
W32_EntityKind;

typedef struct W32_Entity W32_Entity;
struct W32_Entity
{
  W32_Entity *next;
  W32_EntityKind kind;
  union
  {
    struct
    {
      ThreadEntryPointFunctionType *func;
      void *ptr;
      HANDLE handle;
      DWORD tid;
    } thread;
    CRITICAL_SECTION mutex;
    SRWLOCK rw_mutex;
    CONDITION_VARIABLE cv;
  };
};

typedef struct W32_State W32_State;
struct W32_State
{
  Arena *arena;
  SystemInfo system_info;
  CRITICAL_SECTION entity_mutex;
  Arena *entity_arena;
  W32_Entity *entity_free;
};

global W32_State w32_state = {0};

// Must be called once, before anything else in the base layer, on process
// start (explicit - no hidden static-initializer ordering).
internal void os_init(void);

internal W32_Entity *w32_entity_alloc(W32_EntityKind kind);
internal void        w32_entity_release(W32_Entity *entity);
internal DWORD WINAPI w32_thread_entry_point(void *ptr);
internal U32          w32_sleep_ms_from_endt_us(U64 endt_us);

#endif // OS_CORE_WIN32_H
