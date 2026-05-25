#include "bria-frame-protocol.hpp"

#include <cstring>
#include <cstdio>

namespace bria {

namespace {

bool isUnreservedUrlChar(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
	       c == '.' || c == '~';
}

std::string urlEncode(const std::string &value)
{
	std::string encoded;
	encoded.reserve(value.size());

	for (unsigned char c : value) {
		if (isUnreservedUrlChar(static_cast<char>(c))) {
			encoded.push_back(static_cast<char>(c));
		} else {
			char hex[4];
			snprintf(hex, sizeof(hex), "%%%02X", c);
			encoded.append(hex);
		}
	}

	return encoded;
}

} // namespace

std::string buildStreamingWsUrl(const std::string &serverUrl, const std::string &apiToken)
{
	
	if (apiToken.empty()) {
		return {};
	}

	const char separator = serverUrl.find('?') != std::string::npos ? '&' : '?';
	return serverUrl + separator + "api_token=" + urlEncode(apiToken);
}

std::vector<uint8_t> packVideoJpegFrame(uint64_t frameId, int64_t presentationTimestampUs,
					const std::vector<uint8_t> &jpegPayload)
{
	std::vector<uint8_t> out(HEADER_SIZE + jpegPayload.size());

	out[0] = 0x42;
	out[1] = 0x52;
	out[2] = 0x49;
	out[3] = 0x41;
	out[4] = PROTOCOL_VERSION;
	out[5] = APP_REMOVE_BG;
	out[6] = MEDIA_TYPE_VIDEO;
	out[7] = CODEC_JPEG;

	const uint32_t frameIdHi = static_cast<uint32_t>(frameId >> 32);
	const uint32_t frameIdLo = static_cast<uint32_t>(frameId & 0xFFFFFFFFU);
	out[8] = static_cast<uint8_t>((frameIdHi >> 24) & 0xFF);
	out[9] = static_cast<uint8_t>((frameIdHi >> 16) & 0xFF);
	out[10] = static_cast<uint8_t>((frameIdHi >> 8) & 0xFF);
	out[11] = static_cast<uint8_t>(frameIdHi & 0xFF);
	out[12] = static_cast<uint8_t>((frameIdLo >> 24) & 0xFF);
	out[13] = static_cast<uint8_t>((frameIdLo >> 16) & 0xFF);
	out[14] = static_cast<uint8_t>((frameIdLo >> 8) & 0xFF);
	out[15] = static_cast<uint8_t>(frameIdLo & 0xFF);

	const int64_t ptsHi = presentationTimestampUs >> 32;
	const uint32_t ptsLo = static_cast<uint32_t>(presentationTimestampUs & 0xFFFFFFFFLL);
	out[16] = static_cast<uint8_t>((ptsHi >> 24) & 0xFF);
	out[17] = static_cast<uint8_t>((ptsHi >> 16) & 0xFF);
	out[18] = static_cast<uint8_t>((ptsHi >> 8) & 0xFF);
	out[19] = static_cast<uint8_t>(ptsHi & 0xFF);
	out[20] = static_cast<uint8_t>((ptsLo >> 24) & 0xFF);
	out[21] = static_cast<uint8_t>((ptsLo >> 16) & 0xFF);
	out[22] = static_cast<uint8_t>((ptsLo >> 8) & 0xFF);
	out[23] = static_cast<uint8_t>(ptsLo & 0xFF);

	if (!jpegPayload.empty()) {
		std::memcpy(out.data() + HEADER_SIZE, jpegPayload.data(), jpegPayload.size());
	}

	return out;
}

std::optional<BinaryFrame> unpackBinaryFrame(const std::vector<uint8_t> &buffer)
{
	if (buffer.size() < HEADER_SIZE) {
		return std::nullopt;
	}

	if (buffer[0] != 0x42 || buffer[1] != 0x52 || buffer[2] != 0x49 || buffer[3] != 0x41) {
		return std::nullopt;
	}

	const uint32_t frameIdHi = (static_cast<uint32_t>(buffer[8]) << 24) |
				   (static_cast<uint32_t>(buffer[9]) << 16) |
				   (static_cast<uint32_t>(buffer[10]) << 8) |
				   static_cast<uint32_t>(buffer[11]);
	const uint32_t frameIdLo = (static_cast<uint32_t>(buffer[12]) << 24) |
				   (static_cast<uint32_t>(buffer[13]) << 16) |
				   (static_cast<uint32_t>(buffer[14]) << 8) |
				   static_cast<uint32_t>(buffer[15]);

	BinaryFrame frame;
	frame.frameId = (static_cast<uint64_t>(frameIdHi) << 32) | frameIdLo;
	frame.mediaType = buffer[6];
	frame.payload.assign(buffer.begin() + HEADER_SIZE, buffer.end());
	return frame;
}

} // namespace bria
