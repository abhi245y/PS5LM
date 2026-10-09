# Dashboard pages (`feature/dashboard-pages`, 0.3.0)

## Widgets

The home page has eight tiles; some repeat each other:

- MEMORY's ring and GPU MEMORY's bar show the same 11.4 GiB pool (GPU and
  CPU share it).
- GPU's "model on the GPU" and CPU's "CPU heap" lines repeat GPU MEMORY's
  Model and Other segments.
- SESSION and CONTEXT both count tokens.

Proposed eight:

| Tile | Shows |
|---|---|
| GENERATION | tok/s history, min/avg/max, prompt speed (the large tile, unchanged) |
| GPU | busy %, history |
| CPU | use of the app's 13 CPUs, history |
| MEMORY | one tile: % in use and the model / KV cache / other / free bar |
| THERMALS | SoC and CPU temperature, fan duty, SoC power |
| CONTEXT | fill bar, plus tokens in and out, replies and uptime (SESSION folded in) |
| MODEL | small card; Cross opens a detail view (all arguments, sampling, layers, trained context, cache); the library moves to its own action |
| STORAGE | free space on /data and on USB, models found |

THERMALS: Marice/ps5-exporter reads `sceKernelGetCpuTemperature`,
`sceKernelGetSocSensorTemperature`, `sceKernelGetCurrentFanDuty` (0..1024)
and `sceKernelGetSocPowerConsumption` from a payload. First check each
resolves inside a title (some imports are null in titles, as `isatty` was).
If the fan is not exposed, ship temperatures only.

## Pages, switched with L1/R1

1. **Dashboard**: the tiles above.
2. **Settings**: stored in `/data/PS5LM/settings.json`. Default model and
   auto-load at launch, context cap, KV cache type override, sampling
   overrides per model, the server port, sounds on or off.
3. **Logs**: `app.log` and `llama.log`, scrollable, newest at the bottom,
   with a filter for errors.

Reuse ps5-homebrew-ui's `concepts/settings.cpp` and `concepts/terminal.cpp`
before writing new components.

## Done when

On the console: the eight tiles show live values (THERMALS at least
temperatures), L1/R1 move between the three pages, a setting changed on the
Settings page survives a restart and takes effect, and the Logs page scrolls
the live log. The PC preview renders every page.

## Risks

- Thermal calls may not resolve in a title.
- Reading a large `llama.log` every frame: read new bytes only, keep the last
  few thousand lines.
