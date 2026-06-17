# OBS Plugin: Bria Background Removal

An OBS Studio plugin that removes the background from your video in real time using [Bria's](https://bria.ai)
cloud-based AI API — no local ML models required.

## Requirements

- OBS Studio 31.1.1 or later
- A Bria account (sign up at [bria.ai](https://bria.ai))
- Internet connection (background removal runs on Bria's cloud)

## Installation

### Windows

1. Download the latest `.zip` from the [releases page](../../releases).
2. Extract to `%ProgramFiles%\obs-studio\`.
3. Restart OBS Studio.

### macOS

1. Download the latest `.pkg` from the [releases page](../../releases).
2. Run the installer.
3. Restart OBS Studio.

### Linux

1. Download the latest `.deb` from the [releases page](../../releases).
2. `sudo dpkg -i obs-backgroundremoval-*.deb`
3. Restart OBS Studio.

## Usage

1. In OBS, right-click a video source → **Filters** → **+** → **Bria - Remove Background**.
2. Click **Sign in with Bria** and complete the browser login.
3. Once signed in, background removal activates automatically on every frame.

## How it works

Frames are JPEG-encoded and streamed to Bria's background removal API over a persistent WebSocket connection. Returned
alpha masks are composited CPU-side, and the final BGRA frame (transparent background) is uploaded to a GPU texture for
OBS rendering.

## Building from source

### Dependencies

| Library                 | Purpose                              |
|-------------------------|--------------------------------------|
| OBS Studio 31.1.1+      | Host API                             |
| Qt 6                    | UI (sign-in dialog, welcome screen)  |
| OpenCV (core + imgproc) | Frame buffer handling                |
| libjpeg-turbo           | JPEG encode/decode for Bria frames   |
| ixwebsocket             | WebSocket streaming to Bria API      |
| MbedTLS (mbedcrypto)    | AES-256-CBC decryption of SSO tokens |
| libcurl                 | Update checker HTTP requests         |

### Build (Windows example)

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
```

See the [CI workflow](.github/workflows/) for macOS and Linux build steps.

## License

> SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>  
>
> SPDX-License-Identifier: GPL-3.0-or-later 
