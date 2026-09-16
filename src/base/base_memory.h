// Virtual memory reserve/commit/decommit/release: implemented per-OS
// (see src/os/core/win32/os_core_win32.c).

#ifndef BASE_MEMORY_H
#define BASE_MEMORY_H

//- @per_os_impl
internal void *reserve_memory(U64 size);
internal B32   commit_memory(void *ptr, U64 size);
internal void  decommit_memory(void *ptr, U64 size);
internal void  release_memory(void *ptr, U64 size);

#endif // BASE_MEMORY_H
