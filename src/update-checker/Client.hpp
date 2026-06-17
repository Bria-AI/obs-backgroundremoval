// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <functional>

void fetchStringFromUrl(const char *urlString, std::function<void(std::string, int)> callback);
