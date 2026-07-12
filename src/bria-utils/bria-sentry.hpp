// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: CC0-1.0

#pragma once
#include <cstdint>
#include <string>

namespace BriaSentry {

void setUser(const std::string &orgId);
void clearUser();

// #1 — WebSocket non-1000 close
void captureWsDisconnect(int code, const std::string &reason, int reconnectCount);

// #2 — WebSocket transport-level error
void captureWsError(const std::string &reason);

// #3 — SSO token decrypt failed
void captureAuthDecryptFailed(const std::string &stage);

// #4 — SSO auth poll returned an unexpected server error
void captureAuthPollError(const std::string &serverError);

// #5 — SSO token renewal failed (user will be silently logged out)
void captureAuthRenewalFailed();

// #6 — OBS shader effect file failed to load (filter silently broken)
void captureShaderLoadFailed(const std::string &path, bool fileExists, const std::string &logMessage,
			     const std::string &deviceName, const std::string &deviceType,
			     const std::string &driverVersion);

// #7 — Caught std::exception in a path that should never throw
void captureException(const std::string &location, const std::string &what);

// #8 — Periodic FPS report (every 60 s while filter is active)
void captureFpsReport(double submittedFps, uint64_t framesSubmitted, uint64_t framesDropped);

} // namespace BriaSentry
