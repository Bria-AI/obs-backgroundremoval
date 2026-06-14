// SPDX-FileCopyrightText: 2021-2026 Roy Shilkrot <roy.shil@gmail.com>
// SPDX-FileCopyrightText: 2023-2026 Kaito Udagawa <umireon@kaito.tokyo>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FILTERDATA_H
#define FILTERDATA_H

#include <obs-module.h>

#include <opencv2/core.hpp>

#include <atomic>
#include <mutex>

/**
  * @brief Shared GPU capture/render state for video filters (Bria-only build).
  *
  * To re-enable ORT filters (Background Removal + Enhance Portrait), restore
  * this file to include models/Model.hpp + ort-utils/ORTModelData.hpp and
  * have filter_data inherit from ORTModelData.
  */
struct filter_data {
	obs_source_t *source = nullptr;
	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stagesurface = nullptr;

	cv::Mat inputBGRA;

	std::atomic<bool> isDisabled{false};

	std::mutex inputBGRALock;
	std::mutex outputLock;
};

#endif /* FILTERDATA_H */
