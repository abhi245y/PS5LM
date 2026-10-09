# Changelog

Versions follow [semver](https://semver.org). The app's version is set in
`ps5/app/version.hpp` and shown on the dashboard; each release is tagged
`v<version>` and ships `ps5lm-app-v<version>.zip` (see `scripts/release-app.sh`).
v0.1.x are the upstream payload releases by cobanov.

## [Unreleased]

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
