# Research notes

What is known about the platform and the prior work, as of October 2026, with sources.

## The console

- **CPU:** 8 Zen 2 cores (16 threads) at up to 3.5 GHz, 3.85 GHz on the Pro. AVX2, FMA, F16C, BMI2; no AVX-512.
- **Memory:** 16 GB GDDR6 shared by CPU and GPU, 448 GB/s peak (576 GB/s on the Pro). How much of the CPU side bandwidth a process sees is a Phase 0 measurement.
- **GPU:** RDNA 2 class, programmed through Sony's private AGC interface. There is no public graphics or compute API.
- **OS:** a FreeBSD derivative. Big allocations go through Sony's direct memory (`sceKernelAllocateDirectMemory`, `sceKernelMapDirectMemory`). In a native app the flexible memory pool behind ordinary allocations is small (see PS5SX2 below); for a payload process, Phase 0 will tell.

## The jailbreak

[Relapse](https://github.com/ntfargo/Relapse-Exploit) (ntfargo, 29 September 2026) covers firmware 7.00 to 13.60 on the PS5 and the PS5 Pro. It starts in the browser (a JavaScriptCore info leak and a structured clone object-pool mismatch that corrupts a typed array) and reaches kernel read/write through a use-after-free race in `aio_multi_wait`. It does not survive a reboot. Firmware 14.00 (16 September 2026) closes it.

On top of it: kstuff (fake-signed executables), a HEN (etaHEN or OnionHEN) and [elfldr](https://github.com/ps5-payload-dev/elfldr), which runs ELF payloads sent to port 9021. Tools from [ps5-payload-dev](https://github.com/ps5-payload-dev): ftpsrv (2121), klogsrv (3232), shsrv (2323), gdbsrv, websrv (8080).

## The SDK

[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) v0.43 (29 August 2026). Clang's `x86_64-sie-ps5` target with any host LLVM from 15 to 21; macOS works with Homebrew's LLVM. Ships libc++, a CMake toolchain and Meson cross files. Payloads take arguments through elfldr as `file:/path/to/elf?args=...`.

## Prior work this project builds on

**[PS5SX2](https://github.com/Swordpdf/PS5SX2)**, PCSX2 on the PS5 (September 2026). The best public notes on the console's memory as a native app sees it:

- `mmap` in the app sandbox fails; flexible memory (`sceKernelMapFlexibleMemory`) is only ~190 to 448 MiB
- large data goes in direct memory; its own fork of the ps5vk driver offers a 12 GiB device heap with `PS5VK_WIDE_MEMORY` (a PS5SX2 switch, not one of PS5_Vulkan's)
- JIT and other rights come from the HEN: the app writes `{"PID":n}` to `/download0/etahen_jailbreak`

**[ProsperoAI](https://github.com/blackbearreloaded/ProsperoAI)** (GPL-3.0), LLM chat on the PS5 GPU (September 2026). Runs Qwen3.5 9B Q4_0 (6.37 GiB) at about 30 tok/s decode and Mistral 7B Q4_0 at 25 to 29 tok/s, with a 4,096 token context. It writes AGC kernels by hand for each model (`src/backends/qwen35`, `src/backends/mistral`) from per-model recipes and supports Q4_0 only, so a new model or quant means new kernels. It vendors llama.cpp's CPU libraries built for the PS5. Proof that a 6+ GiB model and fast GPU decode work on this console.

**[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** (Mihawk-99, GPL-3.0) builds two Vulkan drivers for the PS5's GPU. The one going forward is RADV, Mesa 26.2's AMD driver, from the author's fork [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) with a PS5 winsys over the console's AGC functions. It reports Vulkan 1.4 and compiles shaders with ACO on the console when a pipeline is created. ps5vk, the first driver (Vulkan 1.1), is the one built on the PSBC compiler from the ps5-opengl SDK. vkQuake, RetroArch's Vulkan cores and ProsperoEden run on RADV. For ggml-vulkan it has what is required (Vulkan 1.2 or later, 16-bit storage, timeline semaphores, subgroup operations, fp16, `maxStorageBufferRange` of 4 GiB) but no cooperative matrix and no accelerated integer dot product, so matrix products take the fp16 shaders. A submit that makes no progress for 10 s ends in `VK_ERROR_DEVICE_LOST` (`radv_ps5_winsys.c`). Its measurements are from firmware 6.02 to 12.70, mostly on a PS5 Pro; 13.60 is untested.

**[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)** (GPL-3.0): the build of a native app (`eboot.bin` as an fSELF, `sce_sys/param.json`, `sce_module/libc.prx`) for ShadowMountPlus.

## Lessons from other homebrew

Read from the code, fix commits and closed issues of ps5-payload-dev (sdk, elfldr, websrv, ftpsrv, SDL, pacbrew-repo), Payload Manager, PS5 Game Compressor, etaHEN, kstuff, PS5SX2, PS5CEMU-HAR, ProsperoAI, ps5-opengl and PS5_Vulkan, in October 2026. What PS5LM took from them, and where it came from:

| Lesson | Source | In PS5LM |
|---|---|---|
| After rest mode a listening socket can go stale without an error; rebuild the server on `SIGCONT`, on a new address, and when it stops answering | Payload Manager #61, fixed in cd9ad1f; websrv e5a6369; ftpsrv 4f7408a | ps5lm's watchdog |
| Set only `SO_REUSEADDR`; cpp-httplib's default `SO_REUSEPORT` lets a second copy bind the same port | ftpsrv, websrv, Payload Manager servers | ps5lm's single-instance check |
| Name the process with `thr_set_name`; Payload Manager finds payloads by that name | ftpsrv, websrv, shsrv, Payload Manager | ps5lm |
| Never let the PS5 browser cache a homebrew page | Payload Manager #40 and 3efc4f4 | `Cache-Control: no-store` |
| A wrong console clock shows up as TLS failures | Payload Manager #23, #45, #54 | the download error says so |
| `statfs` on `/data` reports more free space than Settings | PS5 Game Compressor #43 | 512 MiB kept spare |
| Sync data before recording it done or renaming it | PS5 Game Compressor (`fsync` before `rename`) | download piece record |
| Reset the auto power-down timer during long work | PS5 Game Compressor `gc_power_guard.c` | during downloads |
| mbedTLS needs `THREADING_C` and `THREADING_PTHREAD` for concurrent TLS 1.3 | mbedTLS docs; pacbrew's mbedtls package | `build-mbedtls.sh` |
| Pin with `scePthreadSetaffinity` inside the given mask, never widen it, and keep the SMT sibling free; raise only threads that mostly sleep | PS5SX2 (`GSRenderer.cpp`), PS5CEMU-HAR (`port/ps5/threads.cpp`) | `patches/0002` |
| A pinned thread's children inherit its mask | PS5SX2 f9abb4a | main thread left unpinned |
| `std::thread`'s default stack is small; a 64 KB frame overflowed it | PS5SX2 30814bd | `spawn()` keeps 4 MiB stacks |
| Do not run exit-time destructors behind live threads | SDK `crt.c` and f19dd3e; ProsperoAI and the boilerplate never return from `main` | `ps5/compat/exit.cpp` |

For the native app (#4), the projects agree on one recipe for big memory: `sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len, 2 MiB, 12, &phys)`, then `sceKernelMapDirectMemory(&addr, len, 0x33, 0, phys, 2 MiB)` (CPU and GPU read/write), freed with `sceKernelMunmap` and `sceKernelReleaseDirectMemory` (PS5SX2 `pcsx2/Memory.cpp`, PS5_Vulkan `driver/ps5vk_direct_memory.c`, ProsperoAI `src/ps5_agc_backend.cpp`, PS5CEMU-HAR `port/cemu/MemMapperPS5.cpp`). Without an address hint the kernel places mappings first fit in the window the GPU driver needs later, and `vkCreateDevice` then fails (PS5SX2 0ac429f): hint CPU buffers elsewhere, as PS5CEMU-HAR (0x10_0000_0000) and PS5_Vulkan (0x40_0000_0000) do. An app's flexible heap is only 448 MiB configured, so big `std::vector`s and the KV cache belong in direct memory too. ProsperoAI, a native app, maps one block of about 7.38 GiB and adds `amm` and `kernel` page table sizes to its `param.json`. The etaHEN jailbreak request (`{"PID":n}` written to `/download0/etahen_jailbreak.tmp`, then renamed to `/download0/etahen_jailbreak`) needs the title ID on etaHEN's allowlist and gives rights, not memory.

## Qwen 3.8

- **Qwen3.8-27B** (14 August 2026, Apache 2.0): dense, hybrid attention (Gated DeltaNet and gated attention), vision encoder, 262K context, a native MTP block. GGUFs from [unsloth](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF) use the `qwen35` architecture and need llama.cpp from its release week or later.
- **Qwen3.8-Flash-Next**: 180B total, 6B active (MoE, 512 experts).
- **Qwen3.8 2.4T-A95B** and the hosted **Qwen3.8-Max**.
- No model below 27B in this generation.
