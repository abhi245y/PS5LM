# PS5LM roadmap

The goal: llama.cpp running natively on a jailbroken PS5, able to load any GGUF model, with **Qwen 3.8 27B** as the first model to run on the console.

The plan goes CPU first, GPU second. The CPU path needs nothing but the payload SDK and gets Qwen 3.8 talking on the console soonest. The GPU path is where the speed is, and it is the part nobody has done with llama.cpp yet.

## The model and the memory

Qwen 3.8's smallest open model is the 27B (dense, hybrid Gated DeltaNet and gated attention, GGUF architecture `qwen35`). The PS5 has 16 GB of GDDR6 shared by the CPU, the GPU and the system, so only the low-bit quants fit ([unsloth/Qwen3.8-27B-GGUF](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF)):

| Quant | Size | Role |
|---|---|---|
| UD-IQ1_M | 6.27 GiB | fallback if memory is tight |
| **UD-IQ2_XXS** | 6.77 GiB | first light candidate |
| UD-IQ2_S | 7.80 GiB | |
| **UD-Q2_K_XL** | 9.15 GiB | main target: K-quants are the cheapest to decode on CPU and GPU |
| UD-IQ3_XXS | 10.18 GiB | stretch, if a process can hold ~11 GiB |
| UD-Q4_K_M | 15.33 GiB | does not fit |

Only one layer in four keeps a KV cache (the others are Gated DeltaNet with a fixed state), so long contexts stay cheap. The vision encoder (`mmproj`, 0.86 GiB) is left out until the text model runs.

Decoding is bound by memory bandwidth: tokens per second ≈ effective bandwidth / bytes read per token (about the model size). The GDDR6 peaks at 448 GB/s (576 GB/s on the Pro), so the GPU ceiling for Q2_K_XL is around 45 tok/s. What the CPU can pull is what Phase 0 measures.

## Phase 0: toolchain and console facts

- [x] ps5-payload-dev SDK v0.43, pinned by checksum (`scripts/setup-sdk.sh`)
- [x] macOS host toolchain: Homebrew `llvm@21` and `lld` (`scripts/env.sh`)
- [x] `probes/memprobe` builds
- [x] `scripts/host-relapse.sh` serves the exploit page from macOS or Linux; `scripts/console-setup.sh` sends ftpsrv, klogsrv and shsrv after it
- [x] Console jailbroken (Relapse, firmware 13.60 or lower), with ftpsrv (2121), shsrv (2323) and klogsrv (3232)
- [x] memprobe and threadprobe on the console: results in [CONSOLE.md](CONSOLE.md)
  - direct, flexible and malloc ceilings for one payload process
  - the largest single allocation (llama.cpp's CPU backend puts all weights in one buffer)
  - read bandwidth with 1 to 8 threads, on malloc and on direct memory
  - AVX2 FMA GFLOPS
  - file read and mmap speed on a GGUF in `/data/PS5LM/models`

**Done when** we know how many GiB one process can hold and how fast it can read them.

## Phase 1: llama.cpp on the CPU, first light

- [x] ggml and libllama cross-compile with no source changes
- [x] `llama-cli`, `llama-server` and `llama-bench` link ([PORTING.md](PORTING.md) lists the three build fixes)
- [x] Smoke test: Qwen3.5 0.8B generates text on the PS5 CPU, 14.2 tok/s on two threads
- [x] Safe CPU defaults for the PS5 (`patches/0001`): threads = allowed CPUs minus two, no busy-waiting
- [x] `llama-server` on the console with a chat page in the PS5's own browser (`scripts/ps5-chat.sh`, `ps5/webui`); phones and laptops use the same page on port 8081
- [ ] `llama-bench` numbers for the small model
- [ ] **First light: Qwen3.8-27B generates text on the PS5.** UD-Q2_K_XL if Phase 0 says it fits, UD-IQ2_XXS if not. A video of it, posted with the repo.

Expect 1 to 3 tok/s here. Slow, but it is Qwen 3.8 running on a PS5, and the claim we want first.

**Done when** Qwen 3.8 answers a prompt on the console.

## Phase 1.5: the launcher and the model library

An Ollama-like way to get models and chat, without a PC in the loop. One payload, `ps5lm.elf`, kept in the loader's autoload:

- [ ] A small web server (port 8090) with the **model library** page, opened in the PS5's own browser on start (`sceSystemServiceLaunchWebBrowser`, as the SDK's `browser` sample and PS5SX2 do). The same page works from a phone or laptop on the network.
- [ ] The library: the curated list in [MODELS.md](MODELS.md) as `models.json`, each marked fits / tight / too big from the memprobe numbers, plus whatever is already in `/data/PS5LM/models` or on a USB drive.
- [ ] **Download** from Hugging Face straight to `/data/PS5LM/models`, with progress and resume. HTTPS through OpenSSL from ps5-payload-dev's pacbrew repository, or Sony's own `libSceHttp2` and `libSceSsl`. The same TLS turns on llama.cpp's `-hf` downloads.
- [ ] **Run**: start `llama-server` with the chosen model through the local elfldr (`127.0.0.1:9021`), stopping the one before. llama-server's own router mode spawns child processes, which a payload cannot do, so the launcher does the routing.
- [ ] **Chat** in llama-server's built-in web UI, on the TV through the PS5 browser or from any device on the network, and the OpenAI-compatible API for other apps. If the PS5's WebKit is too old for that UI, the launcher serves a plain chat page instead.

**Done when** a model can be picked, downloaded and chatted with using only the DualSense.

## Phase 2: fit and speed on the CPU

- [ ] Weights in direct memory if malloc's ceiling is too low: a ggml CPU buffer type on `sceKernelAllocateDirectMemory`, as a patch in `patches/`
- [ ] Loading: mmap or `--no-mmap`, from `/data` or a USB drive, whichever Phase 0 shows is faster
- [ ] Threads: the best `-t` for the cores a payload is allowed, with pinning
- [ ] Qwen 3.8's native MTP block for speculative decoding
- [ ] Context budget: 8k to 32k tokens

**Done when** Q2_K_XL runs at the best speed the CPU allows, and the numbers are in `docs/CONSOLE.md`.

## Phase 3: the GPU through Vulkan

The route: ggml's Vulkan backend on [Mihawk-99's PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), a RADV based driver that programs the PS5's GPU (AGC) directly and passes the Vulkan CTS. llama.cpp keeps its own kernels, so every model and quant comes along.

- [ ] Build the driver on Linux (an OrbStack Ubuntu machine is enough)
- [ ] App shell: `eboot.bin` with `sce_sys/param.json`, from [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate), mounted by ShadowMountPlus, jailbroken at start through the HEN request PS5SX2 uses
- [ ] ggml-vulkan cross-compiled, its shaders compiled to SPIR-V on the host
- [ ] `test-backend-ops` on the console: every op the `qwen35` graph uses matches the CPU
- [ ] Weights in device memory (the driver's 12 GiB heap, `PS5VK_WIDE_MEMORY`)
- [ ] Qwen3.8-27B decoding on the GPU, target 15 tok/s or more

**Done when** the GPU decodes at least five times faster than the CPU.

## Phase 4: the app

- [ ] A chat screen on the TV, with the DualSense and the system keyboard
- [ ] Model picker over `/data/PS5LM/models` and USB drives
- [ ] `llama-server` always on, so the PS5 serves an OpenAI-compatible API to the home network
- [ ] Releases: payload ELFs and the app folder, with an install guide

**Done when** someone who is not a developer can install it and chat.

## Later

- Qwen3.8-Flash-Next (180B total, 6B active) with experts streamed from the SSD
- Vision input for the 27B (`mmproj`)
- Upstream the small portability fixes to llama.cpp

## Risks

| Risk | Plan B |
|---|---|
| A payload process gets far less than 7 GiB (**confirmed: about 6 GiB, shared with the system**) | Move to the native app shell (Phase 3's) sooner, where direct memory is available; smaller models meanwhile |
| The CPU is too slow for a 27B | Still first light; Phase 3 is the fix |
| ggml-vulkan needs a feature the driver lacks | Its feature switches (`GGML_VK_DISABLE_*`), or fix it in the driver with its author |
| Someone else gets there first | Keep Phase 1 short and publish as soon as it talks |
| A firmware update closes the exploit | Keep the console on 13.60 or lower, automatic updates off |
