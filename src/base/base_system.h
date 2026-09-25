// System info: implemented per-OS (see src/os/core/win32/os_core_win32.c).

#ifndef BASE_SYSTEM_H
#define BASE_SYSTEM_H

typedef struct SystemInfo SystemInfo;
struct SystemInfo
{
  U32 logical_processor_count;
  U64 page_size;
};

//- @per_os_impl
internal SystemInfo *get_system_info(void);

#endif // BASE_SYSTEM_H
