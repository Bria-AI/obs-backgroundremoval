<!--
SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>

SPDX-License-Identifier: Apache-2.0
-->

# AGENTS.md

Bria's **OBS Studio background-removal plugin** (V-RMBG 3.0) — a **native C++** OBS Effect
Filter that streams webcam frames to Bria's inference endpoint and renders the
background-removed result into the scene. Built with **CMake + vcpkg** for macOS, Windows,
and Linux. Source lives in `src/`; packaging metadata in `buildspec.json` and `data/`.

Project rules live in `.agents/rules/`. Read and apply each:

- `.agents/rules/**`

## Environment

- **CMake** (presets in `CMakePresets.json`) with **vcpkg** (`vcpkg.json`,
  `vcpkg-configuration.json`). Dependencies (OBS sources, obs-deps, Qt6) are pinned in
  `buildspec.json`.
- Toolchains: Xcode/clang (macOS), MSVC (Windows), gcc/clang (Linux). OBS Studio **31.0+**.

## Build

Use the presets — do not pass ad-hoc `cmake` flags:

```
cmake --preset <macos|windows-x64|ubuntu-x86_64>
cmake --build --preset <macos|windows-x64|ubuntu-x86_64>
```

CI uses the matching `*-ci-*` presets. See `scripts/` for the per-platform dep/build helpers
the workflows call.

## Agent Configuration

All agent rules are canonical in `.agents/`. Thin stubs under `.claude/` and `.cursor/` point
back here — never duplicate content.

```
.agents/
└── rules/    Always-on rules — loaded by all agents on startup
```

| Tool | Rules |
|------|-------|
| Claude Code | `.claude/rules/` stubs → `.agents/rules/` (and root `CLAUDE.md` → `AGENTS.md`) |
| Cursor | `.cursor/rules/*.mdc` → `.agents/rules/` |

### Rules (always-on)

| Rule | Enforces |
|------|----------|
| `code-style` | clang-format, gersemi (CMake), REUSE/SPDX, preset-based builds, Conventional Commits |

## Checks — keep them green

| Check | What it gates |
|-------|---------------|
| `check` | pre-commit hooks: clang-format, gersemi, REUSE, file hygiene, conventional commits |
| `plugin-build-{macos,ubuntu,windows}` | the plugin compiles on every target platform |
| `pr-title` | PR title is a valid Conventional Commit |
| `security` | dependency / code scanning |

Locally, install the hooks once with `pre-commit install` (config: `.pre-commit-config.yaml`);
they run the same formatters and checks on the files you touch.

## Release

Releases are automated by **release-please** (`release-please-config.json`,
`.release-please-manifest.json`): pushes to `main` open/update a release PR that bumps the
version (in `CHANGELOG.md`, `buildspec.json`, `data/manifest.json`). **Merging that PR** cuts
the tag; the tag build (`plugin.yml`) compiles all platforms and attaches the artifacts to the
GitHub Release. Do not hand-edit the version files — release-please owns them.

A CI build can also be triggered manually from Slack (the bot's *Plugins → Build* action
`workflow_dispatch`es `plugin.yml`), or with `gh workflow run plugin.yml`.

See `README.md` for install and usage.
