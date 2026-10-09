# Using the console from coding tools

The app's llama-server speaks three APIs on port 8081, so most coding agents
can use the model on the PS5 as their backend:

| API | Path | Used by |
|---|---|---|
| OpenAI Chat Completions | `/v1/chat/completions` | opencode, aider, Continue, most others |
| OpenAI Responses | `/v1/responses` | codex |
| Anthropic Messages | `/v1/messages` | Claude Code |

Replace `192.168.1.19` with your console's address (the dashboard shows it).
The model name in a request is ignored: the server answers with the model
loaded in the library, so any name works. Tool calling uses the model's own
chat template (llama-server's Jinja templates are on by default).

## Which model

Agents send long prompts (system prompt, tool definitions, files), and
prompt processing on the console runs at 35 to 50 tokens a second for the
27B. A 20,000-token agent prompt takes several minutes before the first word.
For agents, a smaller model with a long context is the better trade:

- Qwen 3.8 27B: best answers, slow on long prompts; keep requests small.
- Gemma 4 E4B or another 4 to 9B model: several times faster on prompts.

Give the tool the context length the dashboard shows (the CONTEXT tile), so it
does not send more than the server holds.

## opencode

`~/.config/opencode/opencode.json`:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "ps5lm": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "PS5LM",
      "options": { "baseURL": "http://192.168.1.19:8081/v1" },
      "models": { "ps5": { "name": "Model on the PS5", "limit": { "context": 65536, "output": 8192 } } }
    }
  }
}
```

Then `opencode`, and pick **PS5LM / Model on the PS5** with `/models`.

## codex

`~/.codex/config.toml`:

```toml
[model_providers.ps5lm]
name = "PS5LM"
base_url = "http://192.168.1.19:8081/v1"
wire_api = "responses"

[profiles.ps5]
model_provider = "ps5lm"
model = "ps5"
```

Then `codex -p ps5`.

## Claude Code

```sh
ANTHROPIC_BASE_URL=http://192.168.1.19:8081 ANTHROPIC_AUTH_TOKEN=ps5lm \
ANTHROPIC_MODEL=ps5 ANTHROPIC_SMALL_FAST_MODEL=ps5 claude
```

## aider

```sh
aider --openai-api-base http://192.168.1.19:8081/v1 --openai-api-key ps5lm --model openai/ps5
```

## Anything else

Point an OpenAI-compatible client at `http://192.168.1.19:8081/v1` with any
API key and any model name.

## Checked on the console

| Tool | Version | Model | Result |
|---|---|---|---|
| opencode | 1.18.35 | | not yet run |
| codex | 0.161.0 | | not yet run |
| Claude Code | | | not yet run |
