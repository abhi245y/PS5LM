# Changelog

Versions follow [semver](https://semver.org). The app's version is set in
`ps5/app/version.hpp` and shown on the dashboard; each release is tagged
`v<version>` and ships `ps5lm-app-v<version>.zip` (see `scripts/release-app.sh`).
v0.1.x are the upstream payload releases by cobanov.

## [Unreleased]

## [0.8.4] - 2026-10-17 (pre-release)

### Changed
- The app reserves 16 GiB of its own space on the console's SSD (`downloadDataSize`), where
  settings and downloads go when nothing opens `/data` to it. Consoles where `/data` is open
  keep using `/data/PS5LM` as before.
- The content ID is `UP9000-PPSA99581_00-PS5LMPLUS0000001`. Install it over 0.8.3 by copying
  the new folder over the old one, as usual.

### Fixed
- Storage on consoles whose HEN does not open `/data` to the title on its own (OnionHEN;
  cobanov/PS5LM#16). The app now asks when it starts, in every process (the restart for a model
  starts a new one), by writing `{"PID":n}` to `/download0/etahen_jailbreak`, which OnionHEN
  honours for a title on its app jailbreak allowlist. When `/data` stays closed the app keeps its
  state and log in its own `/download0` (`/mnt/sandbox/PPSA99581_000/download0/PS5LM` over FTP)
  instead of starting without them; it waits 10 s for the HEN the first time and 2 s after a
  refusal. Models in the title's own folder, `/data/homebrew/PPSA99581/models`, are found with or
  without the HEN's help.
- A first-start choice when `/data` stays closed, with the firmware shown (and logged at every
  start): get full storage and come back, through a current ShadowMountPlus (v1.7-beta5fix1 mounts
  `/data` and `/mnt/usb0` into homebrew titles itself) or the OnionHEN allowlist (the app closes;
  the choice returns until storage opens), or go on in the app's own space. On that route,
  downloads go to `/download0/PS5LM/models`, shown as **App storage** with its real size and free
  space (ps5-exporter lists the download data image as `/mnt/sandbox/download/<title>`), so a
  download that does not fit is refused before it starts. Title-folder models are listed as
  **App folder**.
- `/data/PS5LM` is created when missing, not only `models` under it.

## [0.8.3] - 2026-10-13

### Changed
- The app is now called **PS5LM Plus**, to tell it apart from Mert Cobanov's
  original PS5LM payload: on the home screen, in the dashboard header and in
  the console's notifications. The title ID (PPSA99581), the data folder
  (`/data/PS5LM`) and the repository stay as they were, so updating keeps
  settings, models and downloads.
- The dashboard header shows the chat's address without "Chat at http://",
  so it clears the page tabs with any IP address.

## [0.8.2] - 2026-10-13

### Changed
- `contentVersion` in `sce_sys/param.json` now follows the app's version
  (major+1, minor, patch: 0.8.2 is `01.008.002`). It had stayed at
  `01.000.000` since the first release, so stores such as ProsperoStore
  could not tell an installed copy was out of date. `scripts/release-app.sh`
  refuses to package a release whose `contentVersion` doesn't match.
- README: new screenshots from v0.8.1 (Qwen 3.8 27B generating, the library,
  the loaded-model panel) and a shorter text.

## [0.8.1] - 2026-10-13

### Fixed
- The chat server stopped answering after the network dropped: when the
  console's link went down, accept() failed with `SCE_NET_EINACTIVEDISABLED`
  (errno 163, the same `0x804101a3` a dropped download reported), cpp-httplib
  took that as fatal and closed the listening socket, and the model stayed
  loaded with nothing to reach it. llama-server now binds again once a second
  until it works (`patches/0005-listener-survives-network-drops.patch`).
  Checked on the console: the listener came back within a second after each
  of dozens of drops. A reply streaming at the moment of a drop is still cut.

### Added
- README: the web UI with a sample chat.

### Found
- The repeated "Ethernet disconnected" notifications are the console's link
  going down and up (`eth0: link state changed to DOWN` in the kernel log,
  up to eight times in a few seconds), with or without PS5LM running: a cable,
  port or switch problem, not the app.

## [0.8.0] - 2026-10-13

### Added
- Downloads page (the fifth on L1/R1): a queue kept in
  `/data/PS5LM/downloads.json`, each entry with its speed, time left and
  state. Cross pauses or resumes (or tries a failed one again), Square twice
  cancels and deletes the partial file, or clears a finished one.
- Downloads survive a dropped connection, a pause and the app restarting: the
  file is fetched in 32 MiB pieces on two connections, the part done from its
  start is saved with the queue, and a lost connection is retried on its own
  (4, 8, 16, 32, then 60 s apart, up to 20 times in a row). The SHA-256 is
  checked once the file is whole.
- Get models: Square checks whether the file under the cursor fits, and the
  row says CHECKING while its header is read.
- A busy screen (a spinner and what is happening) while a model is switched or
  unloaded, before the restart for "unload and download", and while the
  models are planned again after a context or cache change.
- README rewritten around the app: features, the payloads it needs, controls.
  The payload's README moved to `docs/PAYLOAD.md`.

### Fixed
- A download that lost its connection failed and threw away what it had
  (2.0 of 10.4 GB, `sceHttp2ReadData 0x804101a3`); now it carries on.
- A connection that went quiet could hang a download: requests now time out
  after 30 s without data and are asked again.
- Settings froze the app for seconds on every change, each one reading every
  model's header again. Only the context and cache settings re-plan now, in
  the background behind the busy screen; the others apply at once.

### Measured (download speed on the console)
- The PC on the same network: 19.6 MB/s from Hugging Face on one connection.
- libSceHttp2 (HTTP/2), one connection: 2.8 to 5.9 MB/s, varying more from run
  to run than with any setting. Reads of 256 KiB gave 0.65 MB/s (each read
  waits about half a second), 4 MiB and larger 3 to 6 MB/s. Four requests on
  one template share one connection (3.5 to 6 MB/s); on four templates, made
  one at a time, 2.8 MB/s; on two, 4.2 MB/s. Made in four threads at once,
  the templates crashed the app.
- libSceHttp2 takes at most 4 requests (512 KiB pool); 8 or 16 are refused
  (`0x817b121c`).
- libSceHttp (HTTP/1.1): 0.44 MB/s per connection whatever the read or receive
  block size, adding up with more connections (4: 1.6, 8: 2.5 MB/s, one of
  the eight refused).
- In the app, two pieces at once on their own templates: 7.0 to 9.8 MB/s for
  a 1.9 GiB file (v0.7: 3.8 MB/s), still well under the PC's speed. Going
  further needs sockets and TLS of our own, for a later release.

### Checked on the console
- A 1.9 GiB download: paused (the partial file stopped growing), resumed from
  the saved 128 MiB, closed and relaunched mid-download (carried on from
  905 MiB), finished with its SHA-256 matching.
- Get models during a download: search, listing and fit checks work.
- The busy screen on a context change; the library re-planned behind it.
- Payloads stayed up throughout.

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
