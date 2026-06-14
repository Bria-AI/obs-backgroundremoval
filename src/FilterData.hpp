// SPDX-FileCopyrightText: 2021-2026 Roy Shilkrot <roy.shil@gmail.com>
// SPDX-FileCopyrightText: 2023-2026 Kaito Udagawa <umireon@kaito.tokyo>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef FILTERDATA_H
#define FILTERDATA_H

#include <obs-module.h>

#include <opencv2/core.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "models/Model.hpp"
#include "ort-utils/ORTModelData.hpp"

/**
  * @brief Shared base for all video filter structs.
  *
  * Inherits ORTModelData for ONNX Runtime session state (session, tensors,
  * input/output names/dims) used by the ORT-based background-removal and
  * enhance-portrait filters.  The Bria streaming filter inherits this struct
  * for the shared GPU capture state but does not use the ORT members.
  */
struct filter_data : public ORTModelData {
	obs_source_t *source = nullptr;
	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stagesurface = nullptr;

	cv::Mat inputBGRA;

	std::atomic<bool> isDisabled{false};

	std::mutex inputBGRALock;
	std::mutex outputLock;

	std::unique_ptr<Model> model;
	std::string useGPU;
	uint32_t numThreads = 1;
	std::string modelSelection;
#ifdef _WIN32
	std::wstring modelFilepath;
#else
	std::string modelFilepath;
#endif
};

#endif /* FILTERDATA_H */
