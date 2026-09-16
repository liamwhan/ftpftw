// Compiler / OS / architecture detection. Mirrors raddebugger's
// base_context_cracking.h, trimmed to the compiler/OS/arch combinations this
// project actually targets (MSVC or Clang, Windows, x64).

#ifndef BASE_CONTEXT_CRACKING_H
#define BASE_CONTEXT_CRACKING_H

////////////////////////////////
//~ Clang OS/Arch Cracking

#if defined(__clang__)

# define COMPILER_CLANG 1

# if defined(_WIN32)
#  define OS_WINDOWS 1
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
#  define ARCH_X64 1
# else
#  error Only x64 is supported.
# endif

////////////////////////////////
//~ MSVC OS/Arch Cracking

#elif defined(_MSC_VER)

# define COMPILER_MSVC 1

# if defined(_WIN32)
#  define OS_WINDOWS 1
# else
#  error This compiler/OS combo is not supported.
# endif

# if defined(_M_AMD64)
#  define ARCH_X64 1
# else
#  error Only x64 is supported.
# endif

#else
# error Compiler not supported. This project builds with MSVC or Clang.
#endif

////////////////////////////////
//~ Arch Cracking

#if defined(ARCH_X64)
# define ARCH_64BIT 1
# define ARCH_LITTLE_ENDIAN 1
#else
# error Only x64 is supported.
#endif

////////////////////////////////
//~ Language Cracking

#if defined(__cplusplus)
# define LANG_CPP 1
#else
# define LANG_C 1
#endif

////////////////////////////////
//~ Build Option Cracking

#if !defined(BUILD_DEBUG)
# define BUILD_DEBUG 1
#endif

////////////////////////////////
//~ Zero All Undefined Options

#if !defined(ARCH_64BIT)
# define ARCH_64BIT 0
#endif
#if !defined(ARCH_X64)
# define ARCH_X64 0
#endif
#if !defined(COMPILER_MSVC)
# define COMPILER_MSVC 0
#endif
#if !defined(COMPILER_CLANG)
# define COMPILER_CLANG 0
#endif
#if !defined(OS_WINDOWS)
# define OS_WINDOWS 0
#endif
#if !defined(LANG_CPP)
# define LANG_CPP 0
#endif
#if !defined(LANG_C)
# define LANG_C 0
#endif

#if !ARCH_X64
# error You tried to build with an unsupported architecture. Only x64 is supported.
#endif

#endif // BASE_CONTEXT_CRACKING_H
