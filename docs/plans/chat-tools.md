# Chat tools (`feature/chat-tools`, 0.5.0)

## Work

- **Tools in the browser chat:** llama-server's web UI with tool calling
  (`--jinja`), and tools served by the app: create, read and list files,
  fetch a URL, run a calculation. Files live in a scratch folder.
- **Scratch folder:** `/data/PS5LM/scratch` by default (changeable in
  Settings). Settings shows its size and has a Clear button; the app notifies
  when it passes a limit (default 2 GiB).
- **Images:** vision models with their `mmproj` file (Gemma 4, Qwen VL),
  loaded together; the planner counts the projector's memory. The library
  pairs a model with its `mmproj` file automatically.

## Done when

On the console: a chat creates a file through a tool and it appears in the
scratch folder; a picture sent to a vision model gets a correct description;
the size warning fires when the folder passes the limit, and Clear empties it.

## Risks

- Writes to `/data` from the app run at 1 to 2 MB/s (measured); fine for
  chat files, not for large outputs.
- Tools that run code are out of scope: no shell on the console.
