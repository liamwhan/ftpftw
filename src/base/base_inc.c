#include "base_core.c"
#include "base_arena.c"
#include "base_strings.c"
#include "base_threads.c"
#include "base_ring.c"
#include "base_thread_context.c"

#if OS_WINDOWS
# include "../os/core/win32/os_core_win32.c"
#else
# error Operating system backend not found for base layer.
#endif
