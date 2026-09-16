#ifndef BASE_INC_H
#define BASE_INC_H

#include "base_context_cracking.h"

#include "base_core.h"
#include "base_system.h"
#include "base_memory.h"
#include "base_arena.h"
#include "base_strings.h"
#include "base_net.h"
#include "base_threads.h"
#include "base_ring.h"
#include "base_thread_context.h"

#if OS_WINDOWS
# include "../os/core/win32/os_core_win32.h"
#else
# error Operating system backend not found for base layer.
#endif

#endif // BASE_INC_H
