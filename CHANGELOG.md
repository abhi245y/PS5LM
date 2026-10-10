# Changelog

Versions follow [semver](https://semver.org). The app's version is set in
`ps5/app/version.hpp` and shown on the dashboard; each release is tagged
`v<version>` and ships `ps5lm-app-v<version>.zip` (see `scripts/release-app.sh`).
v0.1.x are the upstream payload releases by cobanov.

## [Unreleased]

## [0.7.0] - 2026-10-10

### Added
- Downloads check free space first: the file and 1 GiB to spare must fit on
  the drive. When it does not, a dialog offers the drive with the most room
  for this one download; the download itself refuses too, for the
  after-restart path.
- M.2 and every mounted USB drive: Download to in Settings steps through the
  drives that are mounted (internal, `/mnt/ext0`, `/mnt/usb0..3`), the
  STORAGE tile and the usage details list each one, and the library groups
  models by drive. Models in `PS5LM/models` on the M.2 drive are found too.
- Before a download while a model holds the memory (under 2 GiB free), a
  dialog offers to unload it first: the app restarts without the model and
  the download starts on its own.

### Fixed
- Storage figures were made up: a title's `statvfs` is a stub that reports
  64 GiB with 16 GiB free for every path, and `statfs` is not exported to
  titles. Free and total space now come from ps5-exporter (bundled since
  v0.4.0), which reads the kernel's; internal storage is `/user`. Without the
  exporter the space shows as unknown and does not block a download.
- Get models opened after a restart now lists the popular repositories.
- A download without a checksum survives the unload-and-download restart.

### Checked on the console
- Real space: internal 166.7 of 627.6 GiB free, USB 461 of 477 GiB, as
  ps5-exporter reports them; the drive's 33 MB second partition is left out.
- A 500 GiB job refused on internal storage; a download to the USB drive;
  Download to stepping between internal and USB, saved; unload and download
  (SmolLM2 Q2_K); the scratch size warning and Clear.
- No M.2 drive is fitted, so that path is checked in the preview only.

## [0.6.0] - 2026-10-10

### Added
- Get models page (the fourth on L1/R1): Hugging Face's GGUF repositories,
  most downloaded or by family (L2/R2), each file's size and whether it fits,
  planned from its first 16 MiB, and downloads to internal storage or USB
  (Settings), SHA-256 checked, added to the library when done. HTTPS through
  the system's own libSceHttp2 and libSceSsl. Checked on the console:
  SmolLM2-135M F16 (271 MB) at 3.4 MB/s.
- Chat tools: the browser chat's model can read, write, edit and search files
  (llama-server's built-in tools), confined to `/data/PS5LM/scratch` by
  `patches/0003-tools-root.patch`. Settings has a switch, a size warning
  (default 2 GiB) and a Clear action.
- Images: a vision model's `mmproj` file beside it is loaded with it
  (`--mmproj`), and the planner counts its memory.
- The planner can plan from the first bytes of a GGUF (a range request), the
  base of the model market's fit column.

### Measured
- Writes from the app: stdio 18.5 MB/s, write() 232 MB/s on /data (USB: 293
  and 621 MB/s). Downloads use write().
- Chat tools on the console: a write to /data/homebrew lands in the scratch
  folder; relative paths start there. Gemma 4 E4B with its projector
  described a test image correctly.

## [0.5.0] - 2026-10-10

### Added
- `docs/CLI.md`: opencode, codex and Claude Code against the model on the
  console, each checked on a small edit with Qwen3.8-27B UD-IQ2_XXS.

### Fixed
- Claude Code got HTTP 500 on every request: it sends system messages in the
  middle of a conversation and Qwen's template refuses them.
  `patches/0004` folds them into the first system message.

## [0.4.0] - 2026-10-10

### Added
- POWER card: the SoC's power (CPU, GPU and memory together), its history,
  energy used this session and joules per generated token; expanded, min,
  average and max, fan duty and every temperature sensor. The reading is
  `sceKernelGetSocPowerConsumption`'s low 32 bits in milliwatts (22 W idle,
  86 W while the 27B generates), through ps5-exporter.
- USAGE card: GPU and CPU on one chart; expanded, GPU, CPU, memory, storage
  and the models on disk.

### Changed
- USAGE replaces the GPU and CPU tiles; THERMALS shows the hottest sensor.

### Fixed
- Speed and GPU use during a long reply: llama-server's counters move only
  when a request ends, so the live figures come from the slot's token count.

## [0.3.0] - 2026-10-10

### Added
- Pages on L1/R1: Dashboard, Settings and Logs, with tabs in the header.
- Settings page (rail and rows after the kit's Control Room design): load a
  model at launch and which one, the planner's longest context and KV cache
  type, interface sounds; saved to `/data/PS5LM/settings.json`.
- Logs page: `app.log` and `llama.log`, scrollable, with an errors-only filter.
- Model details on Cross over the Model tile: every llama-server argument, the
  plan, cache and preset; Triangle opens the library.
- THERMALS tile (SoC and CPU temperature, fan duty), STORAGE tile (free space
  on /data and USB), and the CPU clock. A title may not read the sensors
  (`0x80020002`), so the app bundles Marice's ps5-exporter (GPL-3.0, pinned
  v0.2.0) and sends it to elfldr when it is not running.
- `scripts/ps5lm-app.sh press <button>`: drive the screens from a PC.

### Changed
- One MEMORY tile replaces MEMORY and GPU MEMORY (the GPU and CPU share one
  pool); CONTEXT takes in the session counts.

## [0.2.0] - 2026-10-10

First release of the native app (title PPSA99581), from this fork.

### Added
- Native PS5 app running llama-server on the GPU through ggml-vulkan and RADV,
  with an 11.44 GiB unified device heap.
- Dashboard on the TV (ps5-homebrew-ui): generation speed, GPU busy, CPU use,
  memory pool, GPU memory split, session, context and model tiles.
- Model library with internal storage and USB drive sections; models load
  straight from USB.
- Planner that picks context length and KV cache type for the memory free.
- Sampling presets for Qwen 3.5/3.6/3.8, Gemma 4, gpt-oss, Granite, Nemotron,
  Mistral and Llama.
- Model switching by restarting the app (in-place unload leaks GPU memory).
- `scripts/ps5lm-app.sh` to drive the app from a PC, and the version on the
  dashboard and in the log.

### Fixed
- Unloading during a reply no longer hangs the app: it waits at most 10 s for
  llama-server before restarting.

### Changed
- A plain launch loads no model and opens the library.
