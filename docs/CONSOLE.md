# Console facts

Measured on the project's test console. Payload numbers come from `probes/memprobe` and `probes/threadprobe`, run through shsrv.

## Setup

- PS5 Slim (CFI-2015), firmware 13.60
- Relapse through WebKit Autoloader v0.5.2 and Payload Manager (PLK)
- Autoload: kstuff-lite 1.11, ShadowMountPlus 1.7beta2, etaHEN 2.5B, ftpsrv 0.21.1, shsrv 0.20, klogsrv 0.9
- FTP upload from a Mac on the same network: about 61 MB/s

The Relapse kernel stage sometimes hangs and the console then shuts down, or sticks on "preparing to turn off" until the power button is held. That is the exploit's race, not anything PS5LM runs.

## Memory, for one payload process

| What | Value |
|---|---|
| Flexible memory configured / free | 8.00 / 7.97 GiB |
| malloc ceiling (256 MiB steps, pages touched) | 6.25 GiB |
| Largest single block (`posix_memalign`) | 6.00 GiB |
| Direct memory | none: size 0, allocation fails with `0x80020023` |

The flexible pool is shared with the system: holding about 6 GiB froze the home screen until the process ended. Plan on 4 to 5 GiB for model, context and buffers in a payload. Bigger models (Qwen 3.8 27B) need a process with direct memory, that is a native app.

## CPU, for one payload process

- AVX, AVX2, FMA, F16C, BMI2; no AVX-512. 16 logical CPUs online.
- **Affinity mask `0xea00`: only CPUs 9, 11, 13, 14 and 15**, shared with system services. Sony priority 700 (normal) for the main thread and new threads.
- 1, 2 and 4 threads busy for a second each: fine.
- Busy-waiting on all five (llama.cpp's defaults: one thread per physical core, `--poll 50`) starved the system: services stopped answering and the console shut down twice. `patches/0001-ps5-safe-cpu-defaults.patch` makes the default thread count the allowed CPUs minus two and turns polling off.

## llama.cpp

First run, Qwen3.5 0.8B Q4_K_M, `-t 2 --poll 0 -lm none`, 512 context:

| | |
|---|---|
| Load | 4.4 s |
| Prompt | 38.5 tok/s |
| Generation | 14.2 tok/s |

`llama-server` loads Granite 4.2 3B (2.1 GiB) with mmap, so mapping a model file works in a payload.

Chat through `llama-server` and `ps5/webui` in the PS5's browser (browser open, `-c 4096 -np 1 -lm none`, 3 threads from the safe defaults):

| Model | Prompt | Generation |
|---|---|---|
| Qwen3.5 0.8B Q4_K_M, thinking off | 67 tok/s | 13.1 tok/s |
| Granite 4.2 3B Q4_K_M | 0.6 tok/s | 0.2 tok/s |

llama.cpp's own web UI renders blank in the PS5 browser; `ps5/webui` (plain ES5) works.

## Payload memory gets paged out

Models over ~1 GB ran 10 to 65 times slower than their size suggests (Granite 4.2 3B at 0.2 tok/s, Qwen3.5 2B at 0.9 tok/s). The reason is paging, not the architecture: while the PS5 browser is in front, the system pages a background payload's memory out to disk. An idle ps5lm was found holding 6 MB of a 600 MB model; the next request paged it back in at 0.3 tok/s.

Locking the weights (`-lm mlock`) fixes it: Qwen3.5 2B went from 0.9 to **8.5 to 9.2 tok/s** and stayed at 1.4 GB resident. Locked pages come out of the pool the home screen needs, though: 1.4 GB locked was fine, **2.7 GB (Qwen3.5 4B) froze the console**. ps5lm locks every model and refuses files over 1.6 GB.

## Downloads

- Sony's `libSceHttp2` cannot send a request from a payload (`sceHttp2SendRequest` fails); plain sockets, DNS and TCP 443 work. ps5lm uses cpp-httplib over mbedTLS 3.6.7 with the Mozilla CA list.
- The kernel caps `SO_RCVBUF` at 64 KB, whatever is asked for. With the Hugging Face CDN ~150 ms away, one connection tops out at 0.9 MB/s.
- 16 connections fetching 8 MiB pieces in parallel: **about 12.5 MB/s** (Qwen3.5 4B, 2.6 GB, in 217 s).

At exit, Sony's libc prints `[SceLibc] A heap error is detected` (SceLibcInternalHeap) after all work is done. Not investigated yet.

## shsrv quirks

- Arguments are split on spaces with no quoting: put prompts in a file (`-f`).
- Programs need an absolute path; a relative one, and very long command lines, end with exit code 3 ("command not found").
- `kill` takes no `-9`; Payload Manager's `/process_kill?pid=N` works.
- `browse <URL>` opens the PS5's web browser.
