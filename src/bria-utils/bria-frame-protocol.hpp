#ifndef BRIA_FRAME_PROTOCOL_H
#define BRIA_FRAME_PROTOCOL_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bria {

constexpr size_t HEADER_SIZE = 24;
constexpr uint8_t PROTOCOL_VERSION = 3;
constexpr uint8_t APP_REMOVE_BG = 1;
constexpr uint8_t MEDIA_TYPE_VIDEO = 1;
constexpr uint8_t CODEC_JPEG = 1;

constexpr const char *DEFAULT_WS_URL = "wss://streaming.prod.bria-api.com";

constexpr int AIMD_INITIAL_MAX_INFLIGHT = 6;
constexpr int AIMD_MAX_INFLIGHT_CEILING = 16;
constexpr double AIMD_SPIKE_THRESHOLD_FACTOR = 2.0;
constexpr double AIMD_BACKOFF_FACTOR = 0.85;
constexpr double AIMD_RECOVERY_INCREMENT = 0.2;
constexpr int AIMD_WARMUP_FRAMES = 8;
constexpr int AIMD_STALE_FRAME_TIMEOUT_MS = 3000;

struct BinaryFrame {
	uint64_t frameId = 0;
	uint8_t mediaType = 0;
	std::vector<uint8_t> payload;
};

std::string buildStreamingWsUrl(const std::string &serverUrl, const std::string &apiToken);

std::vector<uint8_t> packVideoJpegFrame(uint64_t frameId, int64_t presentationTimestampUs,
					const std::vector<uint8_t> &jpegPayload);

std::optional<BinaryFrame> unpackBinaryFrame(const std::vector<uint8_t> &buffer);

} // namespace bria

#endif /* BRIA_FRAME_PROTOCOL_H */
