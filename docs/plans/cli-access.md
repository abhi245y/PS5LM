# CLI access (`feature/cli-access`, 0.4.0)

The console already serves llama-server's OpenAI API at `:8081`, and the
pinned llama.cpp (b11327) also has `/v1/messages` (Anthropic's API). Most of
this goal is making that dependable and documented.

## Work

- A stable model name in the API (`--alias`, from the model's family) so
  tools can be configured once.
- An optional API key (`--api-key`, from Settings), shown on the dashboard.
- Tool calling on: `--jinja` with the model's chat template, checked with a
  tool-call request per preset family.
- `docs/CLI.md`: configuration for opencode, codex, Claude Code
  (`ANTHROPIC_BASE_URL`), t3 code, Continue, aider; with the context length to
  set and what works and what does not.
- The dashboard shows the address and the model name to paste.

## Done when

opencode and Claude Code each complete a small coding task (read a file,
edit it) against the console, and codex at least chats. Logged in
`docs/CLI.md` with the model used.

## Risks

- Long prompts from coding agents: prompt processing runs at 35 to 50 tok/s
  for the 27B, so a 20k-token agent prompt takes minutes. Document which
  models suit agents (smaller, faster ones).
- Only one slot (`-np 1`): concurrent requests queue.
