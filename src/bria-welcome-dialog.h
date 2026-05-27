// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Shows the first-launch welcome dialog.
// Must be called from the Qt main thread (e.g. inside an OBS frontend event callback).
void bria_show_welcome_dialog(void);

#ifdef __cplusplus
}
#endif
