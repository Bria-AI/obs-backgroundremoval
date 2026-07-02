// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "bria-utils/bria-rmbg-client.hpp"

#include <string>

#ifdef __cplusplus
extern "C" {
#endif

// Shows a modal, message-only error dialog for a known WebSocket close reason.
// If serverMessage is non-empty (the server sent a detailed error explanation,
// e.g. a specific quota-exceeded message), it's shown verbatim instead of the
// generic localized text for that reason.
// Closed via the dialog's native title bar close (X) button — no action buttons.
// Must be called from the Qt main thread (e.g. via QMetaObject::invokeMethod).
void bria_show_error_dialog(BriaCloseReason reason, const std::string &serverMessage);

#ifdef __cplusplus
}
#endif
