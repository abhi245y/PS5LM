# Porting notes

What it took to build llama.cpp for the PS5, and why. Upstream is pinned at `b11327` (2026-10-01).

## The target

The SDK compiles with clang's own PS5 target, `x86_64-sie-ps5`, which defines `__PROSPERO__`, `__SCE__` and `__FreeBSD__ 9`. Its CMake toolchain (`prospero-cmake`) presents the console as FreeBSD on x86_64. Programs link dynamically against stubs of Sony's libraries (`libSceLibcInternal`, `libkernel_web`, `libSceNet`, ...) and run as payloads under elfldr.

ggml and libllama build for it as they are. One source patch so far, for safety on the console rather than for building (below).

## Build fixes (all in `scripts/build-llama.sh`)

1. **`posix_spawn_file_actions_addchdir_np` is missing.** The SDK's libc predates it, and `common/subproc.cpp` (through `vendor/sheredom/subprocess.h`) calls it. The header lets the caller turn it off: `-DSUBPROCESS_HAVE_CWD=0`. Nothing on the console spawns processes.
2. **There is no `libm`.** ggml links `m` unless `MATH_LIBRARY` is set; the math functions live in Sony's libc, so `-DMATH_LIBRARY=SceLibcInternal`.
3. **`accept4` is declared but not exported.** The SDK's `sys/socket.h` declares it (FreeBSD 10), libkernel exports only `accept`, and cpp-httplib's server loop calls `accept4(..., SOCK_CLOEXEC)`. `ps5/compat/compat.c` implements it on `accept` and `fcntl`, and goes last on every link line through `CMAKE_C_STANDARD_LIBRARIES` and `CMAKE_CXX_STANDARD_LIBRARIES`.

Build options: AVX, AVX2, FMA, F16C and BMI2 on, AVX-512 off (Zen 2), no OpenMP (ggml's own thread pool), static libraries, no OpenSSL.

## Source patches (`patches/`, applied by `scripts/build-llama.sh`)

1. **`0001-ps5-safe-cpu-defaults.patch`.** A payload may only use 5 of the 16 logical CPUs, and system services share them. llama.cpp's defaults (one thread per physical core, busy-waiting at `--poll 50`) took all five and the console shut down. On `__PROSPERO__` the default thread count becomes the CPUs in the affinity mask (`scePthreadGetaffinity`) minus two, and polling defaults to 0. `-t` and `--poll` still override.

## Output

`build/llama-ps5/bin/`: `llama-cli` (20 MB), `llama-server` (20 MB), `llama-bench` (13 MB), position independent FreeBSD ELFs that import from `libSceLibcInternal.sprx`, `libkernel_web.sprx` and `libSceNet.sprx`. The only weak import is `__cxa_thread_atexit_impl`, which libc++abi checks for before use.

`llvm-readelf` warns that `PT_DYNAMIC` is larger than `.dynamic`: the SDK's linker script puts `.dynsym`, `.dynstr` and `.rela.dyn` in the same segment, and the SDK's own samples have the same layout.

## On the console

Runs: see [CONSOLE.md](CONSOLE.md). mmap of the model works; one block of 6 GiB is the ceiling; `std::thread::hardware_concurrency()` reports 16 while only 5 CPUs are allowed, hence the patch. Open: the `[SceLibc] A heap error is detected` message at exit.
