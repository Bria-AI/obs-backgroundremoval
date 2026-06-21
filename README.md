<!--
SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>

SPDX-License-Identifier: Apache-2.0
-->

# Bria V-RMBG 3.0 - OBS Background Removal Plugin

> Real-time AI background removal for OBS Studio

---

## Overview

The **Bria V-RMBG 3.0 OBS Plugin** adds a native Effect Filter to OBS Studio that removes your video background in real time using Bria's V-RMBG 3.0 model. Your webcam frames are streamed to Bria's inference endpoint, returned background-removed, and rendered directly into your OBS scene - no green screen, no local GPU processing required.

This plugin is built on Bria's commercially licensed AI models, making it safe for any stream, broadcast, or commercial production.

**Key features:**

- �� **Real-time background removal** - live video processed frame-by-frame via Bria's streaming endpoint
- �� **Non-binary alpha edges** - soft, natural cutouts instead of hard masks; hair and fine details blend smoothly
- ��️ **Native OBS filter** - integrates as a standard Effect Filter on any camera source

---

## Requirements

| Requirement         | Details                                                                         |
| ------------------- | ------------------------------------------------------------------------------- |
| OBS Studio          | 31.0 or later - [download here](https://obsproject.com/download)                |
| Bria account        | Required for authentication - [create one here](https://platform.bria.ai/login) |
| Operating system    | Windows 10/11 (x64) · macOS 12+ (arm64 and x86_64)                              |
| Internet connection | Required - frames are processed in Bria's cloud in real time                    |

---

## Installation

### Windows

1. Download the `.zip` from the [Releases page](https://github.com/bria-ai/obs-background-removal/releases).
2. Extract the ZIP and locate the `obs-backgroundremoval` folder inside.
3. Copy it into `C:\ProgramData\obs-studio\plugins\` (create the `plugins` folder if it doesn't exist).
4. Restart OBS Studio.

> **Updating from a previous version?** The ZIP includes `remove-old-installation.bat`. Right-click → Run as administrator before copying the new files.

### macOS

1. Download `obs-backgroundremoval-3.0-macos-universal.pkg` from the [Releases page](https://github.com/bria-ai/obs-background-removal/releases).
2. Open the `.pkg` file and follow the installer steps.
3. Restart OBS Studio.

---

## Getting Started

### 1. Add your camera source

1. In the **Sources** panel, click **+** and choose **Video Capture Device**.
2. Name it, click **OK**, select your webcam, and click **OK** again.

### 2. Apply the Bria background removal filter

1. Right-click your camera source → **Filters**.
2. In the Filters window, click **+** under **Effect Filters**.
3. Choose **Bria – Remove Background** from the list.
4. Click **OK**.

### 3. Sign in to Bria (one time only)

1. In the filter panel, click **Sign in with Bria**.
2. Your browser opens - complete the login at [platform.bria.ai/login](https://platform.bria.ai/login).
3. Return to OBS. The status updates to **"Signed in as [your email]."**
4. Your camera now appears with the background removed. ✅

> Don't have a Bria account yet? [Create one here](https://platform.bria.ai/login) - it only takes a minute.

---

## Reporting Issues

Found a bug or unexpected behavior? Use the **"Report an issue"** link inside the OBS Filters panel (visible when the Bria – Remove Background filter is selected), or [open a GitHub Issue](https://github.com/bria-ai/obs-background-removal/issues) directly.
