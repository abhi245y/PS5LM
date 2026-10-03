# Porting notes

What it took to build llama.cpp for the PS5, and why. Upstream is pinned at `b11327` (2026-10-01).

## The target

The SDK compiles with clang's own PS5 target, `x86_64-sie-ps5`, which defines `__PROSPERO__`, `__SCE__` and `__FreeBSD__ 9`. Its CMake toolchain (`prospero-cmake`) presents the console as FreeBSD on x86_64. Programs link dynamically against stubs of Sony's libraries (`libSceLibcInternal`, `libkernel_web`, `libSceNet`, ...) and run as payloads under elfldr.

ggml and libllama build for it as they are. Two source patches so far, for the console's CPUs rather than for building (below).

## Build fixes (all in `scripts/build-llama.sh`)

1. **`posix_spawn_file_actions_addchdir_np` is missing.** The SDK's libc predates it, and `common/subproc.cpp` (through `vendor/sheredom/subprocess.h`) calls it. The header lets the caller turn it off: `-DSUBPROCESS_HAVE_CWD=0`. Nothing on the console spawns processes.
2. **There is no `libm`.** ggml links `m` unless `MATH_LIBRARY` is set; the math functions live in Sony's libc, so `-DMATH_LIBRARY=SceLibcInternal`.
3. **`accept4` is declared but not exported.** The SDK's `sys/socket.h` declares it (FreeBSD 10), libkernel exports only `accept`, and cpp-httplib's server loop calls `accept4(..., SOCK_CLOEXEC)`. `ps5/compat/compat.c` implements it on `accept` and `fcntl`, and goes last on every link line through `CMAKE_C_STANDARD_LIBRARIES` and `CMAKE_CXX_STANDARD_LIBRARIES`.

Build options: AVX, AVX2, FMA, F16C and BMI2 on, AVX-512 off (Zen 2), no OpenMP (ggml's own thread pool), static libraries, no OpenSSL. mbedTLS is built with `MBEDTLS_THREADING_C` and `MBEDTLS_THREADING_PTHREAD` (`scripts/build-mbedtls.sh`): ps5lm downloads over 16 connections at once, TLS 1.3 handshakes share PSA's global key store, and that store is only thread-safe with both (mbedTLS `docs/architecture/psa-thread-safety`; pacbrew's PS5 package sets them too).

## Leaving a tool (`ps5/compat/exit.cpp`)

After `main` returns, the SDK's start code (`crt/crt.c`) unloads the Sony modules the payload loaded and calls Sony's `exit()`, which runs every C++ static destructor registered with `__cxa_atexit` while other threads may still run. The SDK moved to this in 2024 (commit f19dd3e); before, payloads ended with the `_exit` syscall. `[SceLibc] A heap error is detected` comes at exit, after the tool's work is done, which points at this phase. `ps5/CMakeLists.txt` links `compat/exit.cpp` into the tools with `--wrap=main`: the start code's call lands in `__wrap_main`, which runs the tool's `main`, drains llama.cpp's log thread and stdio, and leaves through `_Exit`. ps5lm never returns from `main` and leaves the same way when asked to quit.

ProsperoAI's libc replacements (`pthread_once`, `strtof`, `fseek`, an mmap allocator for large blocks) are not copied: ProsperoAI is a native app on a clean-room `libc.prx` whose stubs they patch over, while a payload binds to Sony's own libc, and a second allocator would bring exactly the mixed frees the heap check catches.

## Source patches (`patches/`, applied by `scripts/build-llama.sh`)

1. **`0001-ps5-safe-cpu-defaults.patch`.** A payload may only use 5 of the 16 logical CPUs, and system services share them. llama.cpp's defaults (one thread per physical core, busy-waiting at `--poll 50`) took all five and the console shut down. On `__PROSPERO__` the default thread count becomes the CPUs in the affinity mask (`scePthreadGetaffinity`) minus two, and polling defaults to 0. `-t` and `--poll` still override.
2. **`0002-ps5-thread-affinity-and-priority.patch`.** The SDK's compiler defines `__FreeBSD__` but not `__linux__`, so ggml's thread pinning and priority fell into its "unsupported platforms" branch and `-C`, `--cpu-strict` and `--prio` did nothing. On `__PROSPERO__` they go through `scePthreadSetaffinity` and `scePthreadSetprio`. Priority is only lowered (`--prio -1` gives 720; the default is 700, and 256 runs first), the way PS5SX2 and PS5CEMU-HAR only raise threads that mostly sleep. The calling thread is never pinned: threads it starts later inherit its mask, which left a PS5SX2 helper thread stuck on one CPU (PS5SX2 commit f9abb4a). `set_process_priority` refuses to raise anything, since what `setpriority` does to a payload is unknown.

## The build

`ps5/CMakeLists.txt` is the top-level project: it adds llama.cpp as a subdirectory and the `ps5lm` payload next to it. `ps5lm` links `llama-server-impl` and calls `llama_server(argc, argv)` itself, and carries its two pages and the Mozilla CA list inside the binary (`ps5lm_embed`). cpp-httplib is compiled with `CPPHTTPLIB_MBEDTLS_SUPPORT` everywhere, so its classes match in every file that includes it, and mbedTLS (`scripts/build-mbedtls.sh`) is linked last with the compat library.

## Output

`build/llama-ps5/bin/`: `llama-cli` (20 MB), `llama-server` (20 MB), `llama-bench` (13 MB), position independent FreeBSD ELFs that import from `libSceLibcInternal.sprx`, `libkernel_web.sprx` and `libSceNet.sprx`. The only weak import is `__cxa_thread_atexit_impl`, which libc++abi checks for before use.

`llvm-readelf` warns that `PT_DYNAMIC` is larger than `.dynamic`: the SDK's linker script puts `.dynsym`, `.dynstr` and `.rela.dyn` in the same segment, and the SDK's own samples have the same layout.

## On the console

Runs: see [CONSOLE.md](CONSOLE.md). mmap of the model works; one block of 6 GiB is the ceiling; `std::thread::hardware_concurrency()` reports 16 while only 5 CPUs are allowed, hence the patch. Open: whether leaving through `_Exit` ends the `[SceLibc] A heap error is detected` message (#3).
