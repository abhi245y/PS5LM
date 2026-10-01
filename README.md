# PS5LM

**llama.cpp on a jailbroken PlayStation 5.** Run any GGUF language model on the console itself, first on its Zen 2 CPU, then on its GPU.

The first target is **Qwen 3.8** (27B, hybrid Gated DeltaNet attention), the newest open Qwen. No one has run it on a PS5 yet.

> **Status:** llama.cpp runs on a PS5. Qwen3.5 0.8B generates 14 tokens/s on two of the console's CPU cores (firmware 13.60, PS5 Slim). Next: the chat UI in the PS5's browser, then bigger models. See [docs/ROADMAP.md](docs/ROADMAP.md) and [docs/CONSOLE.md](docs/CONSOLE.md).

## Why llama.cpp

Earlier PS5 LLM work hand-writes GPU kernels for one model at a time. PS5LM ports llama.cpp itself, so every architecture and every quantization llama.cpp supports comes along: Qwen, Llama, Mistral, Gemma, from 1-bit to 8-bit. Qwen 3.8's smallest open model is 27B, and only the 2 to 3 bit quants fit in the console's memory, which llama.cpp already has.

## Layout

| Path | What |
|---|---|
| `third_party/llama.cpp` | upstream llama.cpp, pinned as a submodule |
| `ps5/compat` | the few libc functions the console lacks |
| `patches/` | PS5 changes to llama.cpp's source, kept small |
| `probes/` | `memprobe` and `threadprobe`: what a payload gets on the console |
| `scripts/` | SDK setup, the llama.cpp cross build, sending payloads |
| `docs/` | [roadmap](docs/ROADMAP.md), [models that fit](docs/MODELS.md), [porting notes](docs/PORTING.md), [research](docs/RESEARCH.md) |

## Building

Needs the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk) and a host LLVM.

```sh
# macOS
brew install llvm@21 lld socat cmake ninja
# Debian / Ubuntu
sudo apt install clang-18 lld-18 socat cmake ninja-build

git clone --recursive https://github.com/cobanov/PS5LM
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

## Credits

- [llama.cpp / ggml](https://github.com/ggml-org/llama.cpp), which does the actual work.
- [ps5-payload-dev](https://github.com/ps5-payload-dev) (John Törnblom) for the SDK and the ELF loader.
- [PS5SX2](https://github.com/Swordpdf/PS5SX2), [ProsperoAI](https://github.com/blackbearreloaded/ProsperoAI) and [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), whose notes on the console's memory and GPU this project leans on.

## Licence

GPL-3.0-or-later, see [LICENSE](LICENSE). llama.cpp keeps its MIT licence. No Sony SDK files, keys, firmware or model weights are in this repository.

PS5LM is not affiliated with Sony Interactive Entertainment or Alibaba. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment.
