global U64 w32_perf_freq = 0;

internal void
os_init(void)
{
  // system info must be populated before the first arena_alloc() below -
  // arena_alloc_ aligns its reserve size against get_system_info()->page_size.
  SYSTEM_INFO sys_info = {0};
  GetSystemInfo(&sys_info);
  w32_state.system_info.logical_processor_count = sys_info.dwNumberOfProcessors;
  w32_state.system_info.page_size = sys_info.dwPageSize;

  InitializeCriticalSection(&w32_state.entity_mutex);
  w32_state.entity_arena = arena_alloc();

  LARGE_INTEGER freq = {0};
  QueryPerformanceFrequency(&freq);
  w32_perf_freq = (U64)freq.QuadPart;

  WSADATA wsa_data = {0};
  WSAStartup(MAKEWORD(2, 2), &wsa_data);
}

////////////////////////////////
//~ System Info

internal SystemInfo *
get_system_info(void)
{
  return &w32_state.system_info;
}

////////////////////////////////
//~ Virtual Memory

internal void *
reserve_memory(U64 size)
{
  return VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE);
}

internal B32
commit_memory(void *ptr, U64 size)
{
  return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != 0;
}

internal void
decommit_memory(void *ptr, U64 size)
{
  VirtualFree(ptr, size, MEM_DECOMMIT);
}

internal void
release_memory(void *ptr, U64 size)
{
  // NOTE: size unused on Windows (VirtualFree with MEM_RELEASE requires 0),
  // but kept in the signature since other OSes need it.
  (void)size;
  VirtualFree(ptr, 0, MEM_RELEASE);
}

////////////////////////////////
//~ Networking

// Bounds the one blocking call libssh2_session_set_timeout can't reach
// (it only governs libssh2's own I/O, after this function has already
// handed it a connected socket) - against a black-holed address/firewall
// that never resets the connection, the OS's own connect() timeout can be
// tens of seconds to several minutes, freezing whichever thread called
// this (control thread on startup/reconnect, or a transfer/scan thread)
// for that entire span. Non-blocking connect + WSAPoll gives us our own,
// much shorter bound instead.
#define NET_CONNECT_TIMEOUT_MS 15000

internal B32
net_tcp_connect(String8 host, U16 port, U64 *out_socket)
{
  B32 result = 0;
  Temp scratch = scratch_begin(0, 0);

  char *host_cstr = str8_to_cstring(scratch.arena, host);
  String8 port_str = str8f(scratch.arena, "%u", (U32)port);
  char *port_cstr = str8_to_cstring(scratch.arena, port_str);

  struct addrinfo hints = {0};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo *addrs = 0;
  if(getaddrinfo(host_cstr, port_cstr, &hints, &addrs) == 0)
  {
    for(struct addrinfo *addr = addrs; addr != 0; addr = addr->ai_next)
    {
      SOCKET sock = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
      if(sock == INVALID_SOCKET)
      {
        continue;
      }

      u_long nonblocking = 1;
      ioctlsocket(sock, FIONBIO, &nonblocking);

      B32 connected = 0;
      int connect_rc = connect(sock, addr->ai_addr, (int)addr->ai_addrlen);
      if(connect_rc == 0)
      {
        connected = 1;
      }
      else if(WSAGetLastError() == WSAEWOULDBLOCK)
      {
        WSAPOLLFD pfd = {0};
        pfd.fd = sock;
        pfd.events = POLLOUT;
        int poll_rc = WSAPoll(&pfd, 1, NET_CONNECT_TIMEOUT_MS);
        if(poll_rc > 0 && (pfd.revents & (POLLERR | POLLHUP)) == 0)
        {
          int so_err = 0;
          int so_err_len = sizeof(so_err);
          if(getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_err, &so_err_len) == 0 && so_err == 0)
          {
            connected = 1;
          }
        }
      }

      u_long blocking = 0;
      ioctlsocket(sock, FIONBIO, &blocking);

      if(connected)
      {
        // Nagle batches small sends waiting to piggyback on an ACK; libssh2
        // does many small send() calls per SSH packet, so Nagle + delayed
        // ACK stalls every packet by up to ~40-200ms - disabling it is
        // required for SFTP throughput, not an optional tune.
        BOOL nodelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
        *out_socket = (U64)sock;
        result = 1;
        break;
      }
      closesocket(sock);
    }
    freeaddrinfo(addrs);
  }

  scratch_end(scratch);
  return result;
}

internal void
net_close(U64 socket)
{
  closesocket((SOCKET)socket);
}

////////////////////////////////
//~ Entity Pool
//
// A small free-listed pool backing every Thread/Mutex/RWMutex/CondVar
// handle. Lets threads and locks be allocated and released constantly
// (e.g. one thread per transfer) without touching the general-purpose heap.

internal W32_Entity *
w32_entity_alloc(W32_EntityKind kind)
{
  W32_Entity *result = 0;
  EnterCriticalSection(&w32_state.entity_mutex);
  {
    result = w32_state.entity_free;
    if(result)
    {
      SLLStackPop(w32_state.entity_free);
    }
    else
    {
      result = push_array_no_zero(w32_state.entity_arena, W32_Entity, 1);
    }
    MemoryZeroStruct(result);
  }
  LeaveCriticalSection(&w32_state.entity_mutex);
  result->kind = kind;
  return result;
}

internal void
w32_entity_release(W32_Entity *entity)
{
  entity->kind = W32_EntityKind_Null;
  EnterCriticalSection(&w32_state.entity_mutex);
  SLLStackPush(w32_state.entity_free, entity);
  LeaveCriticalSection(&w32_state.entity_mutex);
}

////////////////////////////////
//~ Time

internal U64
now_time_us(void)
{
  LARGE_INTEGER counter = {0};
  QueryPerformanceCounter(&counter);
  return ((U64)counter.QuadPart * 1000000ull) / w32_perf_freq;
}

internal void
sleep_ms(U32 ms)
{
  Sleep(ms);
}

internal U32
w32_sleep_ms_from_endt_us(U64 endt_us)
{
  U32 result = 0;
  if(endt_us == max_U64)
  {
    result = INFINITE;
  }
  else
  {
    U64 now_us = now_time_us();
    if(endt_us > now_us)
    {
      result = (U32)((endt_us - now_us) / 1000);
    }
  }
  return result;
}

////////////////////////////////
//~ Current Thread Info

internal U32
tid(void)
{
  return (U32)GetCurrentThreadId();
}

internal void
set_platform_thread_name(String8 name)
{
  Temp scratch = scratch_begin(0, 0);
  int wide_count = MultiByteToWideChar(CP_UTF8, 0, (char *)name.str, (int)name.size, 0, 0);
  WCHAR *wide = push_array_no_zero(scratch.arena, WCHAR, wide_count + 1);
  MultiByteToWideChar(CP_UTF8, 0, (char *)name.str, (int)name.size, wide, wide_count);
  wide[wide_count] = 0;
  SetThreadDescription(GetCurrentThread(), wide);
  scratch_end(scratch);
}

////////////////////////////////
//~ Thread Functions

internal DWORD WINAPI
w32_thread_entry_point(void *ptr)
{
  W32_Entity *entity = (W32_Entity *)ptr;
  ThreadEntryPointFunctionType *func = entity->thread.func;
  void *thread_ptr = entity->thread.ptr;
  TCTX *tctx = tctx_alloc();
  tctx_select(tctx);
  func(thread_ptr);
  tctx_release(tctx);
  return 0;
}

internal Thread
thread_launch(ThreadEntryPointFunctionType *f, void *p)
{
  W32_Entity *entity = w32_entity_alloc(W32_EntityKind_Thread);
  entity->thread.func = f;
  entity->thread.ptr = p;
  entity->thread.handle = CreateThread(0, 0, w32_thread_entry_point, entity, 0, &entity->thread.tid);
  Thread result = {IntFromPtr(entity)};
  return result;
}

internal B32
thread_join(Thread thread, U64 endt_us)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(thread.u64[0]);
  DWORD wait_result = WAIT_OBJECT_0;
  if(entity != 0)
  {
    wait_result = WaitForSingleObject(entity->thread.handle, w32_sleep_ms_from_endt_us(endt_us));
    CloseHandle(entity->thread.handle);
    w32_entity_release(entity);
  }
  return wait_result == WAIT_OBJECT_0;
}

internal void
thread_detach(Thread thread)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(thread.u64[0]);
  if(entity != 0)
  {
    CloseHandle(entity->thread.handle);
    w32_entity_release(entity);
  }
}

////////////////////////////////
//~ Synchronization Primitives

//- recursive mutexes

internal Mutex
mutex_alloc(void)
{
  W32_Entity *entity = w32_entity_alloc(W32_EntityKind_Mutex);
  InitializeCriticalSection(&entity->mutex);
  Mutex result = {IntFromPtr(entity)};
  return result;
}

internal void
mutex_release(Mutex mutex)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(mutex.u64[0]);
  DeleteCriticalSection(&entity->mutex);
  w32_entity_release(entity);
}

internal void
mutex_take(Mutex mutex)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(mutex.u64[0]);
  EnterCriticalSection(&entity->mutex);
}

internal void
mutex_drop(Mutex mutex)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(mutex.u64[0]);
  LeaveCriticalSection(&entity->mutex);
}

//- reader/writer mutexes

internal RWMutex
rw_mutex_alloc(void)
{
  W32_Entity *entity = w32_entity_alloc(W32_EntityKind_RWMutex);
  InitializeSRWLock(&entity->rw_mutex);
  RWMutex result = {IntFromPtr(entity)};
  return result;
}

internal void
rw_mutex_release(RWMutex rw_mutex)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(rw_mutex.u64[0]);
  w32_entity_release(entity);
}

internal void
rw_mutex_take(RWMutex rw_mutex, B32 write_mode)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(rw_mutex.u64[0]);
  if(write_mode) AcquireSRWLockExclusive(&entity->rw_mutex);
  else            AcquireSRWLockShared(&entity->rw_mutex);
}

internal void
rw_mutex_drop(RWMutex rw_mutex, B32 write_mode)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(rw_mutex.u64[0]);
  if(write_mode) ReleaseSRWLockExclusive(&entity->rw_mutex);
  else            ReleaseSRWLockShared(&entity->rw_mutex);
}

//- condition variables

internal CondVar
cond_var_alloc(void)
{
  W32_Entity *entity = w32_entity_alloc(W32_EntityKind_ConditionVariable);
  InitializeConditionVariable(&entity->cv);
  CondVar result = {IntFromPtr(entity)};
  return result;
}

internal void
cond_var_release(CondVar cv)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(cv.u64[0]);
  w32_entity_release(entity);
}

internal B32
cond_var_wait(CondVar cv, Mutex mutex, U64 endt_us)
{
  U32 wait_ms = w32_sleep_ms_from_endt_us(endt_us);
  W32_Entity *entity = (W32_Entity *)PtrFromInt(cv.u64[0]);
  W32_Entity *mutex_entity = (W32_Entity *)PtrFromInt(mutex.u64[0]);
  return SleepConditionVariableCS(&entity->cv, &mutex_entity->mutex, wait_ms) != 0;
}

internal void
cond_var_signal(CondVar cv)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(cv.u64[0]);
  WakeConditionVariable(&entity->cv);
}

internal void
cond_var_broadcast(CondVar cv)
{
  W32_Entity *entity = (W32_Entity *)PtrFromInt(cv.u64[0]);
  WakeAllConditionVariable(&entity->cv);
}

//- semaphores (thin wrapper over a raw HANDLE - no entity pool needed since
//  Win32 already hands us an opaque handle here)

internal Semaphore
semaphore_alloc(U32 initial_count, U32 max_count)
{
  HANDLE handle = CreateSemaphoreW(0, initial_count, max_count, 0);
  Semaphore result = {(U64)handle};
  return result;
}

internal void
semaphore_release(Semaphore semaphore)
{
  CloseHandle((HANDLE)semaphore.u64[0]);
}

internal B32
semaphore_take(Semaphore semaphore, U64 endt_us)
{
  DWORD wait_result = WaitForSingleObject((HANDLE)semaphore.u64[0], w32_sleep_ms_from_endt_us(endt_us));
  return wait_result == WAIT_OBJECT_0;
}

internal void
semaphore_drop_count(Semaphore semaphore, U64 drop_count)
{
  ReleaseSemaphore((HANDLE)semaphore.u64[0], (LONG)drop_count, 0);
}
