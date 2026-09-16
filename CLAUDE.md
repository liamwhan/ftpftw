# FTP Client — Project Guide

A native, data-oriented SFTP client, for personal use, connecting to the
user's own servers via SFTP with normal username/password logon. The goal: fast, small, and
actually pleasant to use — no Electron, no OOP frameworks, no hidden
allocations. Plain FTP/FTPS is explicitly out of scope for now; if that
changes, it gets its own phase rather than being bolted on.

## Philosophy — Data-Oriented Design, no exceptions

- **No OOP, no RAII, no C++ class hierarchies, no inheritance, no virtual
  dispatch, no smart pointers, no exceptions, no operator overloading, no
  templates-as-abstraction.** Plain C-style code (C or a minimal, disciplined
  subset of C++ if ever needed) over structs and functions.
- Think in terms of data layout and transformations, not objects and
  behavior. Structure-of-arrays over array-of-structs when it matters for
  cache behavior (e.g. directory listings, transfer queues).
- No hidden control flow, no hidden allocation, no "magic" (constructors,
  destructors, implicit conversions). If a function allocates or has a side
  effect, that should be visible at the call site.
- Prefer flat, explicit code over abstraction layers. Three similar lines of
  code beat a premature abstraction.
- Global state is fine when it's simple and explicit (thread-local context,
  scratch arenas). Don't invent dependency injection or interfaces to avoid it.

## Reference codebase: `raddebugger` (submodule)

`raddebugger/` is Ryan Fleury / Epic Games' RAD Debugger — vendored as a git
submodule **purely as a reference for engineering conventions**, not as a
dependency we build against or link into this project. Treat it as read-only
documentation of "how to do things," specifically:

- `raddebugger/src/base/` — the base layer to study closely:
  - `base_arena.h/.c` — arena allocator (`Arena`, `arena_alloc`, `arena_push`,
    `arena_pop_to`, `Temp` for scratch scopes). This is the memory model we
    want: reserve/commit virtual memory, linear bump allocation, arena chains,
    no `malloc`/`free` per-object.
  - `base_strings.h/.c` — "Ryan's strings": `String8` (`{ U8 *str; U64 size; }`,
    **not** null-terminated), `String8List`/`String8Node` for building strings
    without repeated realloc, `String8Array`, join/split/match helpers. Adopt
    this `String8` shape and API style for all our string handling (paths,
    FTP command/response lines, UI text).
  - `base_core.h/.c`, `base_math.h/.c`, `base_thread_context.h/.c` — base
    types (`U8/U16/U32/U64/S8.../F32/F64/B32`), thread-local scratch arenas
    (`scratch_begin`/`scratch_end`), and general helper macros.
  - `base_command_line.h/.c`, `base_files.h/.c`, `base_log.h/.c` — patterns
    for CLI parsing, file I/O, and logging without OOP ceremony.
- `raddebugger/src/os/` (core + gfx) — the OS abstraction pattern: a thin,
  per-platform-implemented API for virtual memory, threads, windows, and
  input, called uniformly from the rest of the codebase.
- `raddebugger/src/ui/` — immediate-mode GUI construction pattern (hierarchical
  IM UI built each frame from a stable tree of build calls). Our custom IM GUI
  should follow this shape: build the UI tree from state every frame, no
  retained widget objects, no callbacks-as-objects.
- `raddebugger/src/render/` and `draw/` — abstract low-level render backend
  (`R_` namespace) plus a higher-level immediate draw API (`DR_` namespace) on
  top. Our custom renderer should follow the same two-layer split: a thin
  GPU-backend abstraction, and a batching draw-command layer above it.
- `raddebugger/src/base/base_threads.h/.c` — the threading & synchronization
  primitives to mirror exactly (see **Threading Model** below):
  `Thread`/`thread_launch`/`thread_join`, `Mutex`, `RWMutex`, `CondVar`,
  `Semaphore`, `Barrier`, and the `MutexScope(...)` / `RWMutexScope(...)`
  defer-loop macros for scope-based locking without RAII.
- `raddebugger/src/base/base_ring.h/.c` — `Ring` / `GuardedRing`: a
  fixed-size byte ring buffer plus a mutex+condvar-guarded wrapper
  (`guarded_ring_write_or_wait` / `guarded_ring_read_or_wait`). This is the
  cross-thread message-passing primitive: threads talk to each other by
  writing/reading fixed-size structs (or byte blobs) through a guarded ring,
  not by locking and poking each other's state directly.
- `raddebugger/src/base/base_thread_context.h/.c` — `TCTX`: a per-thread
  context bundle (scratch arenas, thread name, progress counters) selected
  via `tctx_select`/`tctx_selected` and accessed through `scratch_begin`/
  `scratch_end`. Every thread we spawn gets one of these, exactly like
  raddebugger.
- `raddebugger/build.bat` — the build script pattern to follow for our own
  `build.bat` (see below): a single script, argument-driven, MSVC and Clang
  both supported, debug by default, `release` for optimized builds.

**Do not modify files under `raddebugger/`.** It's a submodule pinned to
upstream; if it needs updating, that's a deliberate submodule bump, not an
edit.

## Protocol scope: SFTP only

This client speaks **SFTP over SSH**, authenticated with a plain
username/password logon (`libssh2_userauth_password`) — that's the user's
actual use case (their own servers, normal password logon over SFTP). No
key-pair auth, no plain FTP, no FTPS. Keep the protocol layer's API shaped
around that (e.g. don't design an abstract "FTP-or-SFTP" transport
interface pre-emptively, and don't build out public-key auth machinery
that has no caller — build for SFTP + password auth, and only generalize
later if a second protocol or auth method is genuinely committed to).

Since this means a plaintext credential (password) gets typed/stored
somewhere client-side, treat it with care: hold it in as few places as
possible, don't log it, and don't write it to disk unencrypted when the
site-manager/config-persistence phase eventually arrives.

### SSH/SFTP dependency: libssh2

- Vendor **libssh2** (small, C, permissive license) as the SSH/SFTP
  implementation. Do not hand-roll SSH or crypto.
- Build it against **WinCNG** (Windows' native CNG crypto API) as its crypto
  backend, not OpenSSL — this keeps the whole dependency tree to "one C
  library, compiled from source," with no OpenSSL vendoring, matching the
  project's judicious-not-dogmatic stance on dependencies.
- libssh2 gets compiled from source directly by `build.bat`, the same way
  raddebugger builds everything in `third_party/` itself rather than
  fetching prebuilt binaries or using a package manager.

## Threading Model: mirror raddebugger's, from day one

Threading is **not an afterthought bolted on before the GUI phase** — the
very first console version must already use the real threading model,
because the protocol itself demands it: SFTP/SSH needs a persistent
**control connection** (the SSH session: auth, channel setup, directory
ops, transfer requests) running independently from **transfer connections**
(the actual data channels for in-flight uploads/downloads). These are
naturally separate threads from the start, not something to retrofit.

Mirror raddebugger's model exactly rather than reaching for
`std::thread`/`std::mutex`/futures/promises:

- **One thread per connection role.** A dedicated control-connection thread
  owns the SSH session and libssh2 calls for that session (auth, `sftp_*`
  directory/metadata operations, opening new channels for transfers).
  Each active transfer runs on its own thread over its own SFTP channel, so
  a big upload/download never blocks directory listings or new commands on
  the control thread.
- **The control connection lives for the session**, not per-operation: it
  stays connected (servicing on-demand requests - directory navigation
  today, transfer setup later) until the user disconnects or the remote
  end drops it, at which point it reconnects using the stored credentials
  rather than the app just erroring out.
  - **Disconnect detection is reactive by default**: no idle-time
    polling - a dead connection is discovered when the next real
    operation on it fails, then it reconnects transparently and retries.
  - **Exception, once transfers exist**: an *active* transfer should keep
    the connection alive with a keepalive/no-op rather than let it go
    idle mid-transfer and risk a server- or network-side idle timeout.
    Don't build this before there's a transfer thread for it to attach
    to - it's a requirement to remember for that phase, not now.
- **Primitives**: use the raddebugger set as-is —
  `Thread`/`thread_launch`/`thread_join`, `Mutex`/`RWMutex`, `CondVar`,
  `Semaphore` — and the `MutexScope(...)`/`RWMutexScope(...)` defer-loop
  macros for lock scopes. No custom thread-pool abstractions beyond this
  unless a real need shows up.
- **Cross-thread communication via `GuardedRing`, not shared mutable
  state.** The console/UI thread talks to the control thread (and the
  control thread talks to transfer threads) by writing fixed-size command/
  event structs into a `GuardedRing` and reading them back out
  (`guarded_ring_write_or_wait` / `guarded_ring_read_or_wait`), the same
  producer/consumer pattern raddebugger uses to keep its frontend thread and
  worker threads decoupled. Avoid directly locking and mutating another
  thread's state from outside — pass it a message instead.
- **Per-thread context (`TCTX`)**: every thread we spawn (control, each
  transfer) gets its own thread context with scratch arenas, following
  `tctx_alloc`/`tctx_select`, so temporary allocations during a transfer
  never contend with another thread's scratch space.
- **Progress reporting** piggybacks on raddebugger's progress-counter
  pattern (`set_progress`/`add_progress`/`set_progress_target` on the
  thread context) so a transfer thread can report bytes-transferred
  without the console (or later, the GUI) polling libssh2 state directly.

This means even the plain stdout console client in phase 2 has a real
control thread and spins up a real transfer thread per file transfer,
communicating over rings — just printing to stdout instead of drawing a
progress bar. The GUI phase later only has to add a view on top of an
already-concurrent engine, not introduce concurrency into a single-threaded
one.

### Layer / namespace convention (borrowed from raddebugger)

Organize our own `src/` the same way raddebugger organizes its: one folder
per layer, one short prefix per layer used as a poor-man's namespace on
types, functions, and constants (e.g. `sftp_`/`SFTP_` for the SFTP protocol
layer, `ui_`/`UI_` for the GUI layer, `r_`/`R_` for the renderer). Layers form
a DAG — no circular includes between layers. Keep the `base` layer (arena,
strings, core types, threading) dependency-free, exactly like raddebugger's
`base`. `libssh2` lives under a vendored `third_party/` folder, same as
raddebugger's third-party code — a dependency, not a layer we write.

## Build pipeline — keep it a single `build.bat`

Non-negotiable: building this project is **one script**, no CMake, no Ninja,
no generated project files, no package manager. Model it directly on
`raddebugger/build.bat`:

- `build.bat` with no args → debug build of the main client, using MSVC by
  default.
- Simple alphanumeric args unpacked as flags (e.g. `build release`,
  `build clang`, `build asan`).
- Debug is the implicit default; `release` opts into optimizations.
- Both `cl` (MSVC) and `clang` should work as backends.
- Output goes to a `build/` directory that is not checked into version
  control.
- No hidden steps: everything the build does should be visible in
  `build.bat` itself, not tucked away in some other tool's config format.

If a Linux build is ever needed, mirror raddebugger's `build.sh` approach
rather than reaching for a cross-platform build generator.

## Phasing / roadmap

Build console-first: prove out the engine (threading model, SFTP protocol
handling, transfer/queue data structures) against real servers over stdout
before touching a renderer or GUI. The GUI phase should only ever have to
add a *view* onto an already-working, already-concurrent engine.

1. **Base layer** — arena, `String8`, scratch arenas/`TCTX`, threading
   primitives (`Thread`/`Mutex`/`CondVar`/`Semaphore`), `Ring`/`GuardedRing`,
   thin OS abstraction (sockets, files) — all modeled on raddebugger's
   `base`/`os`. No protocol code yet.
2. **Vendor libssh2** — pull in source, build against WinCNG from
   `build.bat`. Confirm it compiles and links standalone before wiring in
   protocol logic.
3. **SFTP console client** — real control-connection thread (username/
   password auth, directory listing, transfer setup) plus a real per-transfer
   thread, talking over `GuardedRing`s, output via stdout. Connect, list,
   upload, download against the user's actual servers. This is where the
   transfer/queue data model gets proven out under real concurrency.
4. **Transfer engine hardening** — multiple concurrent transfers, resume,
   error/retry handling, progress reporting — still console-only.
5. **Renderer + custom IM GUI** — bring up the window/render layer, then
   port the console's data model to the GUI.
6. **Polish** — site manager/bookmarks, config persistence, drag-drop, etc.

### Status

Phases 1-2 and the connect+listing slice of phase 3 are done. Phase 5's
foundation is also done: `src/wm/` (window + event pump), `src/render/`
(D3D11, one instanced-rect pipeline), `src/font/` (`fp_dwrite`/`fnt_cache` -
DirectWrite whole-run shaping, per-glyph atlas cache), `src/draw/`
(`dr_rect`/`dr_text`) — all namespaced `WM_`/`R_`/`FP_`/`FNT_`/`DR_` per the
convention above. `main.c` now runs that GUI loop (not a console loop):
it launches the same unchanged `sftp_control_thread_entry`, drains its
`GuardedRing` non-blockingly once per frame, and renders the connect log +
directory listing as plain text lines in the window - confirmed working
end-to-end against a real server.

One real deviation worth knowing about: `<dwrite.h>` cannot be included
from C at all (`DWRITE_MAKE_OPENTYPE_TAG` unconditionally uses
`static_cast`, even inside an otherwise-C-visible enum - an actual bug in
that SDK header). So `src/font/fp_dwrite.cpp` is a small separate C++
translation unit, compiled and linked as its own step in `build.bat` (not
part of the `main.c` unity build), exposed back to the rest of the
plain-C codebase via an `extern "C"` header (`fp_dwrite.h`) with an opaque
`void*` in place of `IDWriteFontFace*`. Everything else is still one
unity build.

Still outstanding from phase 3/4: actual upload/download (the
per-transfer-thread side of the architecture) - so far only the
control-connection thread and a read-only directory listing exist, both
now proven in both the console and the GUI. No `UI_` widget-tree layer
yet either (no interactivity - can't click into a directory) - add it
when a feature actually needs it, per the DOD philosophy above.

## Style notes

- Types: `PascalCase` for structs/enums/typedefs, `snake_case` for functions
  and variables, short-prefix namespacing per layer (see above), matching
  raddebugger's convention.
- Fixed-width types (`U8/U16/U32/U64/S8/S16/S32/S64/F32/F64/B32`) instead of
  bare `int`/`long`/`bool` — pull these from a `base` layer modeled on
  raddebugger's `base_core.h`.
- Comments only where they explain *why* (a workaround, a non-obvious
  invariant), never restating *what* the code does.
- No exceptions/longjmp for control flow; return codes / explicit error
  values, checked at call sites.
- Avoid defensive code for cases that can't happen. Validate at real
  boundaries only: network input (FTP server responses), file system, user
  input.

## What "joy to use" means here

Performance and simplicity in the code should translate directly into a
snappy, responsive UI: instant directory listings, non-blocking transfers,
no spinner-for-everything UX. When in doubt about a UI or interaction
decision, favor whatever keeps the frame loop simple and the UI immediately
responsive over cleverness in either direction.
