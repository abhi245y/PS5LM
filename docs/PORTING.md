# Porting notes

What it took to build llama.cpp for the PS5, and why. Upstream is pinned at `b11327` (2026-10-01).

## The target

The SDK compiles with clang's own PS5 target, `x86_64-sie-ps5`, which defines `__PROSPERO__`, `__SCE__` and `__FreeBSD__ 9`. Its CMake toolchain (`prospero-cmake`) presents the console as FreeBSD on x86_64. Programs link dynamically against stubs of Sony's libraries (`libSceLibcInternal`, `libkernel_web`, `libSceNet`, ...) and run as payloads under elfldr.

ggml and libllama build for it as they are: no source changes so far.

## Build fixes (all in `scripts/build-llama.sh`)

1. **`posix_spawn_file_actions_addchdir_np` is missing.** The SDK's libc predates it, and `common/subproc.cpp` (through `vendor/sheredom/subprocess.h`) calls it. The header lets the caller turn it off: `-DSUBPROCESS_HAVE_CWD=0`. Nothing on the console spawns processes.
2. **There is no `libm`.** ggml links `m` unless `MATH_LIBRARY` is set; the math functions live in Sony's libc, so `-DMATH_LIBRARY=SceLibcInternal`.
3. **`accept4` is declared but not exported.** The SDK's `sys/socket.h` declares it (FreeBSD 10), libkernel exports only `accept`, and cpp-httplib's server loop calls `accept4(..., SOCK_CLOEXEC)`. `ps5/compat/compat.c` implements it on `accept` and `fcntl`, and goes last on every link line through `CMAKE_C_STANDARD_LIBRARIES` and `CMAKE_CXX_STANDARD_LIBRARIES`.

Build options: AVX, AVX2, FMA, F16C and BMI2 on, AVX-512 off (Zen 2), no OpenMP (ggml's own thread pool), static libraries, no OpenSSL.

## Output

`build/llama-ps5/bin/`: `llama-cli` (20 MB), `llama-server` (20 MB), `llama-bench` (13 MB), position independent FreeBSD ELFs that import from `libSceLibcInternal.sprx`, `libkernel_web.sprx` and `libSceNet.sprx`. The only weak import is `__cxa_thread_atexit_impl`, which libc++abi checks for before use.

`llvm-readelf` warns that `PT_DYNAMIC` is larger than `.dynamic`: the SDK's linker script puts `.dynsym`, `.dynstr` and `.rela.dyn` in the same segment, and the SDK's own samples have the same layout.

## Not verified yet

None of this has run on a console. Things to watch in Phase 0 and 1:

- whether `mmap` of a multi-GiB model file works in a payload, or llama.cpp needs `--no-mmap`
- whether a single `posix_memalign` of 7 to 9 GiB succeeds
- how many cores the payload process may use, and whether `std::thread::hardware_concurrency()` reports them
- stdout buffering through elfldr's socket
