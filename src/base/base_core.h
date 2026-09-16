// Universal, codebase-wide constructs: fixed-width types, keyword macros,
// asserts, atomics, linked-list building macros. No dependencies beyond the
// C standard library. Modeled on raddebugger's src/base/base_core.h, trimmed
// to what this project actually uses.

#ifndef BASE_CORE_H
#define BASE_CORE_H

////////////////////////////////
//~ Foreign Includes

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////
//~ Codebase Keywords

#define internal      static
#define global        static
#define local_persist static

#if COMPILER_MSVC
# define thread_static __declspec(thread)
#elif COMPILER_CLANG
# define thread_static __thread
#else
# error thread_static not defined for this compiler.
#endif

#if COMPILER_MSVC
# define force_inline __forceinline
#elif COMPILER_CLANG
# define force_inline inline __attribute__((always_inline))
#endif

#if COMPILER_MSVC
# define no_inline __declspec(noinline)
#elif COMPILER_CLANG
# define no_inline __attribute__((noinline))
#endif

////////////////////////////////
//~ Units

#define KB(n) (((U64)(n)) << 10)
#define MB(n) (((U64)(n)) << 20)
#define GB(n) (((U64)(n)) << 30)

////////////////////////////////
//~ Branch Predictor Hints

#if defined(__clang__)
# define Expect(expr, val) __builtin_expect((expr), (val))
#else
# define Expect(expr, val) (expr)
#endif
#define Likely(expr)   Expect(expr, 1)
#define Unlikely(expr) Expect(expr, 0)

////////////////////////////////
//~ Clamps, Mins, Maxes

#define Min(A, B) (((A) < (B)) ? (A) : (B))
#define Max(A, B) (((A) > (B)) ? (A) : (B))
#define ClampTop(A, X) Min(A, X)
#define ClampBot(X, B) Max(X, B)
#define Clamp(A, X, B) (((X) < (A)) ? (A) : ((X) > (B)) ? (B) : (X))

////////////////////////////////
//~ For-Loop Construct Macros

#define DeferLoop(begin, end) for(int _i_ = ((begin), 0); !_i_; _i_ += 1, (end))
#define EachIndex(it, count) (U64 it = 0; it < (count); it += 1)

////////////////////////////////
//~ Memory Operation Macros

#define MemoryCopy(dst, src, size) memmove((dst), (src), (size))
#define MemorySet(dst, byte, size) memset((dst), (byte), (size))
#define MemoryCompare(a, b, size)  memcmp((a), (b), (size))

#define MemoryCopyStruct(d, s)  MemoryCopy((d), (s), sizeof(*(d)))
#define MemoryCopyArray(d, s)   MemoryCopy((d), (s), sizeof(d))
#define MemoryCopyStr8(dst, s)  MemoryCopy((dst), (s).str, (s).size)

#define MemoryZero(s, z)      memset((s), 0, (z))
#define MemoryZeroStruct(s)   MemoryZero((s), sizeof(*(s)))

#define MemoryMatch(a, b, z)    (MemoryCompare((a), (b), (z)) == 0)

////////////////////////////////
//~ Asserts

#if COMPILER_MSVC
# define Trap() __debugbreak()
#elif COMPILER_CLANG
# define Trap() __builtin_trap()
#endif

#define AssertAlways(x) do{if(!(x)) {Trap();}}while(0)
#if BUILD_DEBUG
# define Assert(x) AssertAlways(x)
#else
# define Assert(x) (void)(x)
#endif
#define NotImplemented Assert(!"Not Implemented!")
#define InvalidPath    Assert(!"Invalid Path!")
#define StaticAssert(C, ID) global U8 Glue(ID, __LINE__)[(C) ? 1 : -1]

////////////////////////////////
//~ Atomic Operations
//
// Only the handful of operations this project's progress counters and
// free-lists actually need.

#if COMPILER_MSVC
# include <intrin.h>
# define ins_atomic_u64_eval(x)                 (U64)__iso_volatile_load64((__int64*)(x))
# define ins_atomic_u64_eval_assign(x, c)        _InterlockedExchange64((__int64 *)(x), (c))
# define ins_atomic_u64_add_eval(x, c)           _interlockedadd64((__int64 *)(x), (c))
# define ins_atomic_u64_eval_cond_assign(x, k, c) _InterlockedCompareExchange64((__int64 *)(x), (k), (c))
#elif COMPILER_CLANG
# define ins_atomic_u64_eval(x)                 __atomic_load_n((U64 *)(x), __ATOMIC_SEQ_CST)
# define ins_atomic_u64_eval_assign(x, c)        __atomic_exchange_n((x), (c), __ATOMIC_SEQ_CST)
# define ins_atomic_u64_add_eval(x, c)           __atomic_add_fetch((U64 *)(x), (c), __ATOMIC_SEQ_CST)
# define ins_atomic_u64_eval_cond_assign(x, k, c) ({ U64 _new = (c); __atomic_compare_exchange_n((U64 *)(x), &_new, (k), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); _new; })
#endif

////////////////////////////////
//~ Linked List Building Macros

#define CheckNil(nil, p) ((p) == 0 || (p) == nil)
#define SetNil(nil, p) ((p) = nil)

#define SLLStackPush_N(f, n, next) ((n)->next = (f), (f) = (n))
#define SLLStackPop_N(f, next) ((f) = (f)->next)
#define SLLStackPush(f, n) SLLStackPush_N(f, n, next)
#define SLLStackPop(f) SLLStackPop_N(f, next)

#define SLLQueuePush_NZ(nil, f, l, n, next) (CheckNil(nil, f) ? \
((f) = (l) = (n), SetNil(nil, (n)->next)) : \
((l)->next = (n), (l) = (n), SetNil(nil, (n)->next)))
#define SLLQueuePush(f, l, n) SLLQueuePush_NZ(0, f, l, n, next)

////////////////////////////////
//~ Misc. Helper Macros

#define Glue_(A, B) A##B
#define Glue(A, B) Glue_(A, B)

#define ArrayCount(a) (sizeof(a) / sizeof((a)[0]))

#define IntFromPtr(ptr) ((U64)(ptr))
#define PtrFromInt(i) (void*)(i)

#define AlignPow2(x, b) (((x) + (b) - 1) & (~((b) - 1)))

////////////////////////////////
//~ Base Types

typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;
typedef int8_t   S8;
typedef int16_t  S16;
typedef int32_t  S32;
typedef int64_t  S64;
typedef S32      B32;
typedef float    F32;
typedef double   F64;

global U64 max_U64 = 0xffffffffffffffffull;

////////////////////////////////
//~ Fatal Errors

internal void abort_self(S32 code);

#endif // BASE_CORE_H
