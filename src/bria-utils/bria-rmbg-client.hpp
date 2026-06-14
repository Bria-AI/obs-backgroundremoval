#ifndef BRIA_RMBG_CLIENT_H
#define BRIA_RMBG_CLIENT_H

#include <IXWebSocket.h>

#include <opencv2/core.hpp>

#include "bria-frame-protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

class BriaRmbgClient {
public:
	using MaskCallback = std::function<void(cv::Mat foregroundMask, uint64_t frameId)>;
	// Fired on the ixwebsocket thread. connected=true → Open, connected=false → Close.
	using ConnectionCallback = std::function<void(bool connected)>;

	BriaRmbgClient();
	~BriaRmbgClient();

	bool connect(const std::string &apiToken);
	void disconnect();

	bool isConnected() const;
	void setMaskCallback(MaskCallback callback);
	void setConnectionCallback(ConnectionCallback callback);

	// Returns the assigned frameId on success, or UINT64_MAX when throttled/disconnected.
	uint64_t submitFrame(const cv::Mat &imageBGRA, int jpegQuality);

private:
	struct PendingFrame {
		std::chrono::steady_clock::time_point sendTime;
	};

	void handleMessage(const ix::WebSocketMessagePtr &msg);
	void handleBinaryMessage(const std::vector<uint8_t> &data);
	void handleJsonMessage(const std::string &payload);
	void purgeStalePendingFrames();
	void releaseSupersededInFlight(uint64_t arrivedFrameId);
	void updateAimdWindow(double rttMs);
	bool canSend() const;

	ix::WebSocket webSocket_;
	std::string apiToken_;
	MaskCallback maskCallback_;
	std::mutex callbackMutex_;
	ConnectionCallback connectionCallback_;
	std::mutex connectionCallbackMutex_;

	std::atomic<bool> connected_{false};
	std::atomic<uint64_t> nextFrameId_{0};
	std::atomic<int> inFlightCount_{0};
	std::atomic<int> maxInFlight_{bria::AIMD_INITIAL_MAX_INFLIGHT};

	std::mutex pendingMutex_;
	std::unordered_map<uint64_t, PendingFrame> pendingFrames_;

	std::chrono::steady_clock::time_point sessionStart_;
	int aimdWarmupFrames_ = 0;
	double minRttMs_ = 0.0;
	double smoothedRttMs_ = 0.0;
	bool hasRttSamples_ = false;
};

#endif /* BRIA_RMBG_CLIENT_H */
