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
- large data goes in direct memory; the Vulkan driver offers a 12 GiB device heap with `PS5VK_WIDE_MEMORY`
- JIT and other rights come from the HEN: the app writes `{"PID":n}` to `/download0/etahen_jailbreak`

**[ProsperoAI](https://github.com/blackbearreloaded/ProsperoAI)** (GPL-3.0), LLM chat on the PS5 GPU (September 2026). Runs Qwen3.5 9B Q4_0 (6.37 GiB) at about 30 tok/s decode and Mistral 7B Q4_0 at 25 to 29 tok/s, with a 4,096 token context. It writes AGC kernels by hand for each model (`src/backends/qwen35`, `src/backends/mistral`) from per-model recipes and supports Q4_0 only, so a new model or quant means new kernels. It vendors llama.cpp's CPU libraries built for the PS5. Proof that a 6+ GiB model and fast GPU decode work on this console.

**[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)** (Mihawk-99), a Vulkan driver for the PS5's GPU built on Mesa 26.2's RADV. Shaders go SPIR-V → NIR → ACO → AGC (the PSBC compiler from the ps5-opengl SDK). Reports Vulkan 1.4 and 1,569,390 passing CTS cases with no failures; no async compute queues yet. Runs vkQuake at 120 fps in 4K, RetroArch's Vulkan cores and the Eden emulator.

**[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)** (GPL-3.0): the build of a native app (`eboot.bin` as an fSELF, `sce_sys/param.json`, `sce_module/libc.prx`) for ShadowMountPlus.

## Qwen 3.8

- **Qwen3.8-27B** (14 August 2026, Apache 2.0): dense, hybrid attention (Gated DeltaNet and gated attention), vision encoder, 262K context, a native MTP block. GGUFs from [unsloth](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF) use the `qwen35` architecture and need llama.cpp from its release week or later.
- **Qwen3.8-Flash-Next**: 180B total, 6B active (MoE, 512 experts).
- **Qwen3.8 2.4T-A95B** and the hosted **Qwen3.8-Max**.
- No model below 27B in this generation.
