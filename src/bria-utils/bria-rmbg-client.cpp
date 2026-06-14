#include "bria-rmbg-client.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <opencv2/imgproc.hpp>

#include <jpeglib.h>

#include <algorithm>
#include <cmath>
#include <csetjmp>

namespace {

struct JpegErrorMgr {
	jpeg_error_mgr pub;
	jmp_buf setjmpBuffer;
};

static void jpegErrorExit(j_common_ptr cinfo)
{
	auto *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
	longjmp(err->setjmpBuffer, 1);
}

std::vector<uint8_t> encodeFrameAsJpeg(const cv::Mat &imageBGRA, int jpegQuality)
{
	cv::Mat bgr;
	cv::cvtColor(imageBGRA, bgr, cv::COLOR_BGRA2BGR);

	jpeg_compress_struct cinfo;
	JpegErrorMgr jerr;
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = jpegErrorExit;

	if (setjmp(jerr.setjmpBuffer)) {
		jpeg_destroy_compress(&cinfo);
		return {};
	}

	jpeg_create_compress(&cinfo);

	uint8_t *outBuffer = nullptr;
	unsigned long outSize = 0;
	jpeg_mem_dest(&cinfo, &outBuffer, &outSize);

	cinfo.image_width = static_cast<JDIMENSION>(bgr.cols);
	cinfo.image_height = static_cast<JDIMENSION>(bgr.rows);
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, std::clamp(jpegQuality, 30, 95), TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW rowPtr = bgr.ptr<uint8_t>(static_cast<int>(cinfo.next_scanline));
		jpeg_write_scanlines(&cinfo, &rowPtr, 1);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);

	std::vector<uint8_t> result(outBuffer, outBuffer + outSize);
	free(outBuffer);
	return result;
}

cv::Mat decodeMaskJpeg(const std::vector<uint8_t> &jpegData)
{
	if (jpegData.empty()) {
		return {};
	}

	jpeg_decompress_struct cinfo;
	JpegErrorMgr jerr;
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = jpegErrorExit;

	if (setjmp(jerr.setjmpBuffer)) {
		jpeg_destroy_decompress(&cinfo);
		return {};
	}

	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, jpegData.data(), static_cast<unsigned long>(jpegData.size()));
	jpeg_read_header(&cinfo, TRUE);

	cinfo.out_color_space = JCS_GRAYSCALE;
	jpeg_start_decompress(&cinfo);

	const int width = static_cast<int>(cinfo.output_width);
	const int height = static_cast<int>(cinfo.output_height);
	cv::Mat result(height, width, CV_8UC1);

	while (cinfo.output_scanline < cinfo.output_height) {
		JSAMPROW rowPtr = result.ptr<uint8_t>(static_cast<int>(cinfo.output_scanline));
		jpeg_read_scanlines(&cinfo, &rowPtr, 1);
	}

	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return result;
}

} // namespace

BriaRmbgClient::BriaRmbgClient()
{
	webSocket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) { handleMessage(msg); });
}

BriaRmbgClient::~BriaRmbgClient()
{
	disconnect();
}

bool BriaRmbgClient::connect(const std::string &apiToken)
{
	disconnect();

	if (apiToken.empty()) {
		obs_log(LOG_WARNING, "Bria API token is empty");
		return false;
	}

	apiToken_ = apiToken;
	const std::string url = bria::buildStreamingWsUrl(bria::DEFAULT_WS_URL, apiToken_);
	if (url.empty()) {
		return false;
	}

	webSocket_.setUrl(url);
	webSocket_.enableAutomaticReconnection();
	webSocket_.setMinWaitBetweenReconnectionRetries(1000);
	webSocket_.setMaxWaitBetweenReconnectionRetries(5000);

	ix::WebSocketHttpHeaders headers;
	headers["User-Agent"] = "Bria/OBSstudio";
	webSocket_.setExtraHeaders(headers);

	sessionStart_ = std::chrono::steady_clock::now();
	nextFrameId_.store(0);
	inFlightCount_.store(0);
	maxInFlight_.store(bria::AIMD_INITIAL_MAX_INFLIGHT);
	aimdWarmupFrames_ = 0;
	minRttMs_ = 0.0;
	smoothedRttMs_ = 0.0;
	hasRttSamples_ = false;

	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		pendingFrames_.clear();
	}

	webSocket_.start();

	connected_.store(true);
	obs_log(LOG_INFO, "Connecting to Bria streaming RMBG API");
	return true;
}

void BriaRmbgClient::disconnect()
{
	if (!connected_.exchange(false)) {
		return;
	}

	if (webSocket_.getReadyState() == ix::ReadyState::Open) {
		webSocket_.send(R"({"type":"stop"})");
	}

	webSocket_.stop();

	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		pendingFrames_.clear();
	}

	inFlightCount_.store(0);
}

bool BriaRmbgClient::isConnected() const
{
	return connected_.load() && webSocket_.getReadyState() == ix::ReadyState::Open;
}

void BriaRmbgClient::setMaskCallback(MaskCallback callback)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	maskCallback_ = std::move(callback);
}

void BriaRmbgClient::setConnectionCallback(ConnectionCallback callback)
{
	std::lock_guard<std::mutex> lock(connectionCallbackMutex_);
	connectionCallback_ = std::move(callback);
}

bool BriaRmbgClient::canSend() const
{
	return isConnected() && inFlightCount_.load() < maxInFlight_.load();
}

uint64_t BriaRmbgClient::submitFrame(const cv::Mat &imageBGRA, int jpegQuality)
{
	if (!connected_.load() || imageBGRA.empty()) {
		return UINT64_MAX;
	}

	purgeStalePendingFrames();

	if (!canSend()) {
		return UINT64_MAX;
	}

	const std::vector<uint8_t> jpegData = encodeFrameAsJpeg(imageBGRA, jpegQuality);
	if (jpegData.empty()) {
		obs_log(LOG_WARNING, "Failed to encode frame as JPEG for Bria API");
		return UINT64_MAX;
	}

	const uint64_t frameId = nextFrameId_.fetch_add(1);
	const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - sessionStart_);
	const std::vector<uint8_t> packet = bria::packVideoJpegFrame(frameId, elapsedUs.count(), jpegData);

	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		pendingFrames_[frameId] = PendingFrame{std::chrono::steady_clock::now()};
	}

	inFlightCount_.fetch_add(1);
	webSocket_.sendBinary(std::string(reinterpret_cast<const char *>(packet.data()), packet.size()));
	return frameId;
}

void BriaRmbgClient::handleMessage(const ix::WebSocketMessagePtr &msg)
{
	if (!msg) {
		return;
	}

	switch (msg->type) {
	case ix::WebSocketMessageType::Open:
		obs_log(LOG_INFO, "Connected to Bria streaming RMBG API");
		// Restore the connected flag and flush stale state from any previous
		// session so frame submission resumes immediately (fixes silent failure
		// after a server-initiated close such as 1013 capacity-exceeded).
		connected_.store(true);
		inFlightCount_.store(0);
		maxInFlight_.store(bria::AIMD_INITIAL_MAX_INFLIGHT);
		aimdWarmupFrames_ = 0;
		minRttMs_ = 0.0;
		smoothedRttMs_ = 0.0;
		hasRttSamples_ = false;
		{
			std::lock_guard<std::mutex> pLock(pendingMutex_);
			pendingFrames_.clear();
		}
		{
			std::lock_guard<std::mutex> cLock(connectionCallbackMutex_);
			if (connectionCallback_)
				connectionCallback_(true);
		}
		break;
	case ix::WebSocketMessageType::Close:
		obs_log(LOG_INFO, "Disconnected from Bria streaming RMBG API (code %d: %s)", msg->closeInfo.code,
			msg->closeInfo.reason.c_str());
		connected_.store(false);
		{
			std::lock_guard<std::mutex> cLock(connectionCallbackMutex_);
			if (connectionCallback_)
				connectionCallback_(false);
		}
		break;
	case ix::WebSocketMessageType::Error:
		obs_log(LOG_ERROR, "Bria streaming RMBG WebSocket error: %s", msg->errorInfo.reason.c_str());
		break;
	case ix::WebSocketMessageType::Message:
		if (msg->binary) {
			handleBinaryMessage(std::vector<uint8_t>(msg->str.begin(), msg->str.end()));
		} else {
			handleJsonMessage(msg->str);
		}
		break;
	default:
		break;
	}
}

void BriaRmbgClient::handleBinaryMessage(const std::vector<uint8_t> &data)
{
	const std::optional<bria::BinaryFrame> frame = bria::unpackBinaryFrame(data);
	if (!frame.has_value() || frame->mediaType != bria::MEDIA_TYPE_VIDEO) {
		return;
	}

	double rttMs = 0.0;
	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		const auto it = pendingFrames_.find(frame->frameId);
		if (it != pendingFrames_.end()) {
			rttMs = static_cast<double>(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
											it->second.sendTime)
					.count());
		}
	}

	releaseSupersededInFlight(frame->frameId);

	if (rttMs > 0.0) {
		updateAimdWindow(rttMs);
	}

	if (frame->payload.empty()) {
		return;
	}

	const cv::Mat foregroundMask = decodeMaskJpeg(frame->payload);
	if (foregroundMask.empty()) {
		obs_log(LOG_WARNING, "Failed to decode Bria mask JPEG for frame %llu",
			static_cast<unsigned long long>(frame->frameId));
		return;
	}

	std::lock_guard<std::mutex> lock(callbackMutex_);
	if (maskCallback_) {
		maskCallback_(foregroundMask.clone(), frame->frameId);
	}
}

void BriaRmbgClient::handleJsonMessage(const std::string &payload)
{
	if (payload.find("\"type\":\"error\"") != std::string::npos ||
	    payload.find("\"type\": \"error\"") != std::string::npos) {
		obs_log(LOG_ERROR, "Bria streaming RMBG API error: %s", payload.c_str());
	}
}

void BriaRmbgClient::purgeStalePendingFrames()
{
	const auto now = std::chrono::steady_clock::now();
	bool staleFound = false;

	{
		std::lock_guard<std::mutex> lock(pendingMutex_);
		for (auto it = pendingFrames_.begin(); it != pendingFrames_.end();) {
			const auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.sendTime)
						   .count();
			if (ageMs > bria::AIMD_STALE_FRAME_TIMEOUT_MS) {
				staleFound = true;
				it = pendingFrames_.erase(it);
				inFlightCount_.fetch_sub(1);
			} else {
				++it;
			}
		}
	}

	if (staleFound) {
		const int currentMax = maxInFlight_.load();
		maxInFlight_.store(std::max(1, static_cast<int>(currentMax * bria::AIMD_BACKOFF_FACTOR)));
	}
}

void BriaRmbgClient::releaseSupersededInFlight(uint64_t arrivedFrameId)
{
	std::lock_guard<std::mutex> lock(pendingMutex_);
	pendingFrames_.erase(arrivedFrameId);
	inFlightCount_.fetch_sub(1);

	for (auto it = pendingFrames_.begin(); it != pendingFrames_.end();) {
		if (it->first < arrivedFrameId) {
			it = pendingFrames_.erase(it);
			inFlightCount_.fetch_sub(1);
		} else {
			++it;
		}
	}

	if (inFlightCount_.load() < 0) {
		inFlightCount_.store(0);
	}
}

void BriaRmbgClient::updateAimdWindow(double rttMs)
{
	if (aimdWarmupFrames_ < bria::AIMD_WARMUP_FRAMES) {
		aimdWarmupFrames_++;
		return;
	}

	if (!hasRttSamples_) {
		minRttMs_ = rttMs;
		smoothedRttMs_ = rttMs;
		hasRttSamples_ = true;
		return;
	}

	smoothedRttMs_ = 0.9 * smoothedRttMs_ + 0.1 * rttMs;
	minRttMs_ = std::min(minRttMs_, rttMs);

	int currentMax = maxInFlight_.load();
	if (smoothedRttMs_ > minRttMs_ * bria::AIMD_SPIKE_THRESHOLD_FACTOR) {
		currentMax = std::max(1, static_cast<int>(currentMax * bria::AIMD_BACKOFF_FACTOR));
	} else {
		currentMax = std::min(bria::AIMD_MAX_INFLIGHT_CEILING,
				      static_cast<int>(currentMax + bria::AIMD_RECOVERY_INCREMENT));
	}
	maxInFlight_.store(currentMax);
}
