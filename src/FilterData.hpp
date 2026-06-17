//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FILTERDATA_H
#define FILTERDATA_H

#include <obs-module.h>

#include <opencv2/core.hpp>

#include <atomic>
#include <mutex>

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
