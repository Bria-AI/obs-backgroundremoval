# Contributing

Thanks for your interest in contributing to Bria Background Removal for OBS!

## How to contribute

External contributors work through forks and pull requests:

1. **Fork** this repository to your own account.
2. Create a branch in your fork and make your changes.
3. Open a **pull request** against `main`. Direct pushes to `main` are not
   permitted.
4. A member of the Bria team will review and, if accepted, merge your PR.

### What external contributors cannot do

By design, forks and pull requests run with reduced trust:

- They **cannot produce signed or notarized release builds**. Code signing and
  notarization run only on trusted branches/tags maintained by the Bria team.
- They **cannot access `BRIA_SSO_SECRET`** or the macOS signing secrets. These
  are GitHub Actions secrets that are never exposed to pull request builds.

PR builds still compile and run the standard checks so your changes can be
validated.

### Release process

Merging to `main`, tagging, and publishing releases are handled by the Bria
team. Tagged builds are the only ones that embed the SSO secret and produce
signed, notarized artifacts.

## Reporting security issues

Please do not file public issues for security vulnerabilities. See
[SECURITY.md](SECURITY.md) for how to report them privately.

> SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
>
> SPDX-License-Identifier: GPL-3.0-or-later
