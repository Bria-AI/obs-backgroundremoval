// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: CC0-1.0

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

// Classifies a WebSocket close code into a known, user-facing reason.
// Unknown codes map to Unknown, which keeps today's silent-retry behavior
// (no popup, generic "Connecting..." overlay).
enum class BriaCloseReason {
	Unknown,
	Unauthorized,
	GeneralError,
	SessionLimitReached,
	CapacityExceeded,
	SessionTimeout
};

inline BriaCloseReason classifyCloseCode(int code)
{
	switch (code) {
	case 1008:
		return BriaCloseReason::Unauthorized; // "unauthorized"
	case 1011:
		return BriaCloseReason::GeneralError; // server-side internal error
	case 4003:
		return BriaCloseReason::SessionLimitReached; // "session limit reached"
	case 1013:
		return BriaCloseReason::CapacityExceeded; // "capacity exceeded, please try again later"
	case 4008:
		return BriaCloseReason::SessionTimeout; // "session timeout"
	default:
		return BriaCloseReason::Unknown;
	}
}

class BriaRmbgClient {
public:
	using MaskCallback = std::function<void(cv::Mat foregroundMask, uint64_t frameId)>;
	// Fired on the ixwebsocket thread. connected=true → Open (remaining args unused).
	// connected=false → Close: closeCode/closeReason carry the WebSocket close info;
	// serverMessage carries the "message" field from the last JSON error frame the
	// server sent before closing (e.g. a detailed quota-exceeded explanation), or
	// an empty string if the server didn't send one.
	using ConnectionCallback = std::function<void(bool connected, int closeCode, const std::string &closeReason,
						       const std::string &serverMessage)>;

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
	std::atomic<int> reconnectCount_{0};

	std::mutex pendingMutex_;
	std::unordered_map<uint64_t, PendingFrame> pendingFrames_;

	std::chrono::steady_clock::time_point sessionStart_;
	int aimdWarmupFrames_ = 0;
	double minRttMs_ = 0.0;
	double smoothedRttMs_ = 0.0;
	bool hasRttSamples_ = false;

	// "message" field from the most recent JSON error frame, if any. Only ever
	// touched from the ixwebsocket callback thread (handleJsonMessage sets it,
	// the Close case in handleMessage reads-and-clears it), so no locking needed.
	std::string lastErrorMessage_;
};

#endif /* BRIA_RMBG_CLIENT_H */
