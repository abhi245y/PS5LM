<p align="center"><img src="docs/media/app/icon.png" width="128" alt="PS5LM app icon"></p>

# PS5LM

**llama.cpp on a jailbroken PlayStation 5, on its GPU.** Run GGUF language models on the console itself, from a native app with a dashboard on the TV.

> **This is a fork.** PS5LM was created by **[Mert Cobanov](https://github.com/cobanov)** ([cobanov/PS5LM](https://github.com/cobanov/PS5LM), [ps5lm.cobanov.dev](https://ps5lm.cobanov.dev)): the llama.cpp port, the payload, its model library and the research all come from there. This fork, by [Abhiram](https://github.com/abhi245y), adds the **native GPU app** (v0.2.0 and later), offered back upstream in [cobanov/PS5LM#12](https://github.com/cobanov/PS5LM/pull/12).

<p align="center">
  <img src="docs/media/app/dashboard-generating.jpg" width="820" alt="The PS5LM dashboard on the TV while Gemma 4 generates">
</p>

## The native app

A payload gets about 6 GiB of memory and no GPU; a native app (a title) gets the console's 12 GiB of direct memory and the GPU. The app runs llama-server with ggml's Vulkan backend on Mihawk-99's RADV port, with one 11.44 GiB device heap, and serves the chat at `http://<console IP>:8081` (llama.cpp's web UI and the OpenAI API).

| Model (on the PS5 GPU) | Context | Generation |
|---|---|---|
| Qwen3.8-27B UD-Q2_K_XL (9.2 GiB) | 32k, q4_0 cache | 20.3 tok/s; a 7,775-token answer at 19.7 tok/s |
| Qwen3.8-27B UD-IQ2_XXS (6.8 GiB) | 64k, q8_0 cache | 20.7 tok/s |
| Gemma 4 E4B QAT q4_0 (4.8 GiB), from USB | 64k, q4_0 cache | 61 tok/s |
| Llama 3.2 3B Instruct Q4_K_M (1.9 GiB), from USB | 64k, q8_0 cache | 124 tok/s |

For comparison, the 27B on the CPU as a payload, streamed from disk, made about one token a minute.

<table>
  <tr>
    <td><img src="docs/media/app/library.jpg" alt="The model library: internal storage and USB drive sections"></td>
    <td><img src="docs/media/app/dashboard-qwen.jpg" alt="The dashboard with Qwen 3.8 27B UD-Q2_K_XL loaded"></td>
  </tr>
  <tr>
    <td align="center">The model library at launch</td>
    <td align="center">Qwen 3.8 27B loaded</td>
  </tr>
</table>

- **Dashboard:** generation speed, how busy the GPU is, CPU use, the memory pool and how the model, KV cache and the rest split it, context in use, the loaded model, and load progress.
- **Model library:** every `.gguf` in `/data/PS5LM/models` and in `PS5LM/models` on a USB drive, in two sections. Cross loads or unloads. A model on USB loads straight from the drive.
- **Planner:** picks the longest context and the best KV cache type that fit the memory free, for each model.
- **Presets:** sampling settings for Qwen 3.5/3.6/3.8, Gemma 4, gpt-oss, Granite, Nemotron, Mistral and Llama.

### Install the app

You need a jailbroken PS5 on firmware 7.00 to 13.60 with [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) (it registers titles from `/data/homebrew`) and an FTP server such as [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv).

1. Download `ps5lm-app-vX.Y.Z.zip` from this fork's [releases](https://github.com/abhi245y/PS5LM/releases) and check it against its `.sha256`.
2. Unzip it and copy the `PPSA99581` folder to `/data/homebrew/PPSA99581` over FTP. ShadowMountPlus adds **PS5LM** to the home screen.
3. Copy a model to `/data/PS5LM/models`, or put it in `PS5LM/models` on a USB drive (exFAT).
4. Start PS5LM, choose the model in the library, and open `http://<console IP>:8081` on a phone or computer once it says Ready.

Which models fit is in [docs/MODELS.md](docs/MODELS.md); anything llama.cpp runs works if the library shows LOAD.

### What it took

The full notes are in [docs/CONSOLE.md](docs/CONSOLE.md). In short:

- **CPU first:** Qwen 3.8 27B ran as a payload on the Zen 2 CPU, streamed from disk at about a token a minute: proof that llama.cpp works on the console, and that the GPU was needed.
- **A title, not a payload:** llama-server linked into an fSELF `eboot.bin` with a clean-room `libc.prx`, registered through ShadowMountPlus. Titles may not `exit()`, have some libc imports resolve to nothing, and need a working `getaddrinfo`, `accept4` and `HOME`; each was found by a crash.
- **The GPU:** ggml-vulkan on [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)'s RADV with no Vulkan loader, and RADV's unified heap for 11.44 GiB in one heap.
- **The screen:** [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) through ps5-opengl, in the same process as RADV (the two Mesa builds share 4,399 symbol names, so the GL stack is linked with only its API visible).
- **Switching models:** unloading in place leaves about 5 GiB of the pool held, so the app restarts itself for each switch.

### Versions and branches

- Releases follow [semver](https://semver.org): tags `vX.Y.Z`, notes in [CHANGELOG.md](CHANGELOG.md), version set in `ps5/app/version.hpp` and shown on the dashboard. v0.1.x are upstream's payload releases.
- `main` is this fork's integration branch. Each feature is built on its own branch (`feature/...`, `docs/...`) and merged into `main`; the plans are in [docs/plans](docs/plans).
- `native-gpu-app` is the branch of the upstream pull request.

## The payload (upstream v0.1)

The original PS5LM is a payload that needs no install: one `ps5lm.elf` opens a model library in the PS5's browser, for small models (up to 1.6 GB, Qwen3.5 0.8B and 2B). See [docs/ROADMAP.md](docs/ROADMAP.md) and [docs/CONSOLE.md](docs/CONSOLE.md).

<p align="center">
  <a href="https://ps5lm.cobanov.dev"><img src="docs/media/ps5lm-demo.gif" width="640" alt="Qwen3.5 0.8B answering in the PS5's browser, typed with a DualSense"></a>
</p>

### Install the payload

You need a PS5 you own on firmware 7.00 to 13.60, jailbroken, with a payload loader (Payload Manager from WebKit Autoloader, or elfldr on port 9021).

**From Payload Manager**, which also brings updates:

1. *Settings*: turn on **Multiple Payload Sources**.
2. Add the source `https://ps5lm.cobanov.dev/payloads.json`.
3. Install **PS5LM** from the list and load it.

**Or by hand**: download `ps5lm.elf` from the [latest release](https://github.com/cobanov/PS5LM/releases/latest) and load it like any payload (upload it in Payload Manager, or send it to an ELF loader on port 9021).

Once it runs, the PS5's browser opens the PS5LM library. Press **Download** on a model, then **Run**, then **Open chat**. Type with the DualSense.

The library also works from a phone or computer on the same network at `http://<console IP>:8082`, and the chat at port 8081 speaks the OpenAI API. Models are stored in `/data/PS5LM/models`; a GGUF copied there over FTP shows up too.

Limits for now: a payload can only keep about 1.6 GB of model locked in memory without starving the home screen, so the catalog has Qwen3.5 0.8B (13 to 21 tok/s) and Qwen3.5 2B (about 9 tok/s). Bigger models need a native app ([#4](https://github.com/cobanov/PS5LM/issues/4)).

## Why llama.cpp

Earlier PS5 LLM work hand-writes GPU kernels for one model at a time. PS5LM ports llama.cpp itself, so every architecture and every quantization llama.cpp supports comes along: Qwen, Llama, Mistral, Gemma, from 1-bit to 8-bit. Qwen 3.8's smallest open model is 27B, and only the 2 to 3 bit quants fit in the console's memory, which llama.cpp already has.

## Layout

| Path | What |
|---|---|
| `third_party/llama.cpp` | upstream llama.cpp, pinned as a submodule |
| `ps5/compat` | the few libc functions the console lacks |
| `ps5/app` | the native app: llama-server on the GPU through RADV, its icon and `param.json` |
| `patches/` | PS5 changes to llama.cpp's source, kept small |
| `probes/` | `memprobe` and `threadprobe`: what a payload gets on the console |
| `scripts/` | SDK setup, the llama.cpp cross build, sending payloads |
| `docs/` | [roadmap](docs/ROADMAP.md), [models that fit](docs/MODELS.md), [porting notes](docs/PORTING.md), [research](docs/RESEARCH.md) |

## Building from source

Needs the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) and a host LLVM.

```sh
# macOS
brew install llvm@21 lld socat cmake ninja
# Debian / Ubuntu
sudo apt install clang-18 lld-18 socat cmake ninja-build

git clone --recursive https://github.com/abhi245y/PS5LM
cd PS5LM
scripts/setup-sdk.sh
source scripts/env.sh

make -C probes/memprobe          # Phase 0 probe
scripts/build-llama.sh           # llama-cli, llama-server, llama-bench for the PS5
```

## Running on the console

You need a PS5 you own on firmware 7.00 to 13.60, jailbroken, with an ELF loader listening on port 9021 ([elfldr](https://github.com/ps5-payload-dev/elfldr)).

```sh
scripts/host-relapse.sh          # serves the Relapse exploit page; open it in the PS5's browser
export PS5_HOST=192.168.1.50     # your console
scripts/console-setup.sh         # FTP, kernel log and shell payloads
scripts/send.sh probes/memprobe/memprobe.elf
```

The probe streams its results back and also writes them to `/data/PS5LM/memprobe.txt` on the console.

With [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) running too (port 2121), `scripts/run.sh` uploads a llama.cpp tool and starts it with arguments:

```sh
# copy a model to /data/PS5LM/models first (FTP or USB)
scripts/run.sh llama-cli -m /data/PS5LM/models/model.gguf -p "Hello from a PS5" -n 64
scripts/run.sh llama-server -m /data/PS5LM/models/model.gguf --host 0.0.0.0 --port 8081
```

## Building the GPU app

The app links llama.cpp with ggml-vulkan against [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)'s RADV build. Set up PS5_Vulkan and its forks in `.deps/src` (`PS5_Vulkan`, `PS5_Mesa` and `PS5_PayloadSDK` at the revisions PS5_Vulkan pins), then:

```sh
(cd .deps/src/PS5_Vulkan && tools/setup-native-dependencies.sh && tools/build-radv.sh release && make app)
scripts/build-app.sh             # build/app-ps5/title/PPSA99581
```

Copy `build/app-ps5/title/PPSA99581` to `/data/homebrew/PPSA99581` and a model to `/data/PS5LM/models`, then start **PS5LM** from the home screen. The chat is at `http://<console IP>:8081` once the model is on the GPU (about 90 s).

**On the TV** the app shows a dashboard drawn with [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) (a submodule in `third_party/`): generation speed, how busy the GPU is, CPU use, the memory pool the GPU and CPU share and how the model, its KV cache and the rest split it, context in use, and the model. Cross on the Model tile opens the **model library**: every `.gguf` in `/data/PS5LM/models` and on USB drives (`/mnt/usbN` or `/mnt/usbN/PS5LM/models`), each with the context and KV cache type the planner (`ps5/app/model_plan.cpp`) picks for the memory free, and the sampling settings of its family when it is a model we know (Qwen 3.5/3.6/3.8, Gemma 4, gpt-oss, Granite, Nemotron, Mistral, Llama). It lists internal storage and USB drives in separate sections; Cross loads a model from either. llama.cpp reads the whole file into GPU memory (no mmap), so a model on USB loads at the drive's speed and the drive is not touched afterwards.

`/data/PS5LM/app-args.txt` (one argument per line) overrides the planner. The app logs to `/data/PS5LM/app.log` and `llama.log`.

From the PC:

```sh
scripts/preview-app-ui.sh                  # render the dashboard to build/app-preview/*.png (Mesa, no console)
PS5_HOST=<console IP> scripts/ps5lm-app.sh cycle   # close, deploy, launch; also close|deploy|launch
```

The app closes itself on request (`/data/PS5LM/quit`), so nothing is killed while it draws; launching uses [ps5-homebrew-dev-protocol](https://github.com/blackbearreloaded/ps5-homebrew-dev-protocol)'s launch payload, cloned into `.deps/src`.

## Credits

- **[Mert Cobanov](https://github.com/cobanov)**, who created PS5LM ([cobanov/PS5LM](https://github.com/cobanov/PS5LM)): the llama.cpp port to the PS5, the payload and its library, the build scripts and the research this fork builds on.
- [llama.cpp / ggml](https://github.com/ggml-org/llama.cpp), which does the actual work.
- [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) (Mihawk-99) for RADV on the PS5, and [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui), [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) and ps5-homebrew-dev-protocol (BlackBearReloaded) for the app's screen and tooling.
- [ps5-payload-dev](https://github.com/ps5-payload-dev) (John Törnblom) for the SDK and the ELF loader.
- [PS5SX2](https://github.com/Swordpdf/PS5SX2), [ProsperoAI](https://github.com/blackbearreloaded/ProsperoAI) and [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), whose notes on the console's memory and GPU this project leans on.

## Licence

GPL-3.0-or-later, see [LICENSE](LICENSE). llama.cpp keeps its MIT licence. No Sony SDK files, keys, firmware or model weights are in this repository.

PS5LM is not affiliated with Sony Interactive Entertainment or Alibaba. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment.
