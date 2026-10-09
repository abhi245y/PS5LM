# Model market (`feature/model-market`, 0.6.0)

A page that searches Hugging Face (and other GGUF hosts later) and downloads
models to the console, showing whether each one fits before download.

## First: write speed

The app writes to `/data` at about 1 to 2 MB/s sustained (measured on an
import), so a 5 GB model would take about an hour. Before anything else,
measure from the app: raw `write()` against stdio, larger chunks, `O_DIRECT`,
and writing to a USB drive instead. ftpsrv writes to `/data` much faster, so
the limit is in how the app writes or where it is allowed to. Picking the
download folder (USB by default if it is faster) depends on this.

## Work

- **Search:** the Hugging Face API (`/api/models?search=…&filter=gguf`), then
  the repository's file list for each `.gguf` and its size.
- **Fit:** the planner on the file's size and, where the GGUF header can be
  fetched with a range request, its real metadata; rows show the context it
  would get, or TOO BIG.
- **Download:** HTTPS from the app (the SDK's TLS, or a small client),
  resumable with range requests, checked against the file's SHA-256 from the
  API; progress on the page and in a notification.
- **Location:** default `/data/PS5LM/models`, changeable in Settings (USB).
- **Page:** a store-like page (ps5-homebrew-ui's `concepts/store.cpp`), with
  curated picks from the presets first.

## Done when

On the console: search finds a model, the page shows its fit, the download
finishes with a verified checksum, and the model loads from the library.

## Progress

- Done: the planner plans from the first bytes of a GGUF
  (`read_model_header`), so a 16 MiB range request is enough to show a
  model's fit before downloading it; checked against whole-file plans for
  Llama 3.2 3B, Gemma 4 E4B, Qwen 3.8 27B and Granite 3.3 2B.
- Open, needs the console: HTTPS from the title. The app is built without
  TLS (`LLAMA_OPENSSL=OFF`). Two ways: the system's `libSceHttp2` and
  `libSceSsl` (the SDK has their stubs; whether they resolve in a title is
  unknown), or a TLS library built into the app (mbedTLS) with a CA bundle.
  Try the system library first: no certificates to ship.
- Open, needs the console: the write-speed measurement above.

## Risks

- TLS in a title: certificate checks need a CA bundle.
- Write speed, above.
