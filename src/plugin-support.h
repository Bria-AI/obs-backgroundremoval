/*
 *
 * SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

void obs_log(int log_level, const char *format, ...);

#ifdef __cplusplus
}
#endif
