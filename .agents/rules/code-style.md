<!--
SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>

SPDX-License-Identifier: Apache-2.0
-->

# Code Style

Conventions for this **native C++ OBS Studio plugin** (CMake + vcpkg, built for
macOS/Windows/Linux). Everything here is enforced by `.pre-commit-config.yaml` and the
`check` CI workflow — keep both green.

## Formatting

- **C/C++** is formatted by **clang-format** (`.clang-format`, LLVM-derived OBS style).
  Run `clang-format -i` on the files you touch; do not hand-format.
- **CMake** (`CMakeLists.txt`, `*.cmake`) is formatted by **gersemi** (`.gersemirc`).
  Run `gersemi -i <file>`; CI runs `gersemi --check`.
- General whitespace / EOF / line-ending hygiene is handled by the pre-commit hooks
  (`trailing-whitespace`, `end-of-file-fixer`, `mixed-line-ending --fix=lf`). Use **LF**.
- `.editorconfig` is authoritative for indentation and charset — respect it.

## Licensing (REUSE)

- The repo is **REUSE-compliant** — the `reuse` pre-commit hook gates on it. **Every** new
  file needs an SPDX header (`SPDX-FileCopyrightText` + `SPDX-License-Identifier`), inline
  for source or via `.license` / `REUSE.toml` for binaries and generated files.
- Source code is **GPL-3.0-or-later**; tooling/config files added for the repo are
  **Apache-2.0** (match the surrounding files in the same directory).

## C++

- Target the OBS plugin API; keep the filter code allocation- and copy-light on the frame
  path (it runs per video frame). Prefer OBS/`libobs` types and helpers over rolling your own.
- Keep platform-specific code behind the existing CMake/preset guards, not scattered `#ifdef`s.

## Building

- Configure/build via the **CMake presets** (`CMakePresets.json`), not ad-hoc `cmake` flags:
  `cmake --preset <macos|windows-x64|ubuntu-x86_64>` then `cmake --build --preset <same>`.
  CI uses the `*-ci-*` presets. Dependencies are pinned in `buildspec.json` (obs-deps, Qt6)
  and `vcpkg.json`; bump those files rather than fetching versions ad hoc.

## Commits & versioning

- **Conventional Commits** are enforced (the `conventional-pre-commit` commit-msg hook and
  the `pr-title` workflow). Allowed types: `feat`, `fix`, `docs`, `style`, `refactor`,
  `perf`, `test`, `build`, `ci`, `chore`, `revert`.
- Versioning and releases are automated by **release-please** (`release-please-config.json`).
  Do **not** hand-edit `CHANGELOG.md`, the version in `buildspec.json`/`data/manifest.json`,
  or `.release-please-manifest.json` — release-please owns them, driven by your commit types
  (`feat` → minor, `fix` → patch).

```
feat: stream alpha at half-res to cut endpoint bandwidth
fix: release the OBS texture on filter destroy
```

## Before you commit

Install the hooks once: `pre-commit install` (installs both the `pre-commit` and
`commit-msg` hooks). They run clang-format, gersemi, REUSE, file hygiene, and the
conventional-commit check on what you touch — the same gates as CI (`check.yml`).
