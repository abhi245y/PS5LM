# Plans

The next phase, split into one goal per branch. Each branch starts from
`main`, merges back into `main` when its "done when" holds on the console,
and bumps the version in `ps5/app/version.hpp` with a CHANGELOG entry.

| Order | Branch | Plan | Version |
|---|---|---|---|
| 1 | `feature/dashboard-pages`, `feature/power-usage` | [Dashboard pages](dashboard-pages.md): widgets, model details, Settings and Logs pages on L1/R1; USAGE and POWER cards | 0.3.0, 0.4.0 (released) |
| 2 | `feature/cli-access` | [CLI access](cli-access.md): opencode, codex, Claude Code and others against the console | 0.5.0 |
| 3 | `feature/chat-tools` | [Chat tools](chat-tools.md): tools, files, images in the browser chat; a scratch folder with a size warning | 0.6.0 |
| 4 | `feature/model-market` | [Model market](model-market.md): search and download from Hugging Face, with fit shown | 0.7.0 |

`main` is this fork's integration branch. Upstream pull requests go from a
branch cut from `cobanov/PS5LM:main` with only the commits meant for it.
