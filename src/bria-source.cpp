
// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Bria - Remove Background Camera" — a standalone async video source that:
//   1. Reads frames from a selected camera/video source in the scene.
//   2. Sends each frame to the Bria streaming RMBG WebSocket API.
//   3. When the matching mask arrives it composites mask + original frame
//      (both from the SAME moment in time) and outputs the result via
//      obs_source_output_video2.
//
// Because the output frame and its mask are always in sync (matched by
// frameId), there is no visual lag on moving edges.  The output is delayed
// by the network RTT (~100-200 ms) but that delay is perfectly consistent —
// no blinking, no edge drift.

#include "bria-source.h"

#ifdef _WIN32
#include <wchar.h>
#endif

#include <opencv2/imgproc.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <plugin-support.h>
#include <util/platform.h>
#include "bria-utils/bria-auth-client.hpp"
#include "bria-utils/bria-rmbg-client.hpp"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int BRIA_SRC_JPEG_QUALITY = 60;
static constexpr float BRIA_SRC_MASK_THRESHOLD = 0.5f;
static constexpr size_t MAX_BUFFERED_FRAMES = 32;

// ---------------------------------------------------------------------------
// Source data
// ---------------------------------------------------------------------------

struct bria_removal_source {
	obs_source_t *source = nullptr;
	std::string cameraSourceName;

	uint32_t width = 0;
	uint32_t height = 0;

	bool active = false;

	std::unique_ptr<BriaRmbgClient> briaClient;
	std::string lastConnectedToken;
	BriaAuthClient::CallbackHandle authCallbackHandle{0};
	bool authCallbackRegistered{false};

	// Held reference to the currently selected camera source.
	// We call obs_source_inc_active/inc_showing so OBS keeps capturing frames
	// even when the camera source is hidden in the scene.
	obs_source_t *heldCameraSource = nullptr;

	// Frame buffer: frameId → {BGRA pixels, timestamp}
	struct StoredFrame {
		cv::Mat bgra;
		uint64_t timestamp = 0;
	};
	std::unordered_map<uint64_t, StoredFrame> frameBuffer;
	std::mutex frameBufferMutex;

	std::mutex clientMutex;

	~bria_removal_source() { obs_log(LOG_INFO, "Bria source destructor called"); }
};

// ---------------------------------------------------------------------------
// Helper: hold a camera source active (captures frames even when hidden)
// ---------------------------------------------------------------------------

static void setHeldCameraSource(bria_removal_source *s, obs_source_t *newSrc)
{
	if (s->heldCameraSource == newSrc) {
		if (newSrc) {
			obs_source_release(newSrc); // balance the get_source_by_name addref
		}
		return;
	}
	if (s->heldCameraSource) {
		obs_source_dec_active(s->heldCameraSource);
		obs_source_dec_showing(s->heldCameraSource);
		obs_source_release(s->heldCameraSource);
	}
	s->heldCameraSource = newSrc;
	if (newSrc) {
		obs_source_inc_active(newSrc);
		obs_source_inc_showing(newSrc);
		// keep the addref from obs_get_source_by_name — owned by heldCameraSource
	}
}

// ---------------------------------------------------------------------------
// Helper: convert obs_source_frame* to BGRA cv::Mat
// ---------------------------------------------------------------------------

static cv::Mat obsFrameToBGRA(const struct obs_source_frame *frame)
{
	if (!frame || !frame->data[0]) {
		return {};
	}

	const int w = static_cast<int>(frame->width);
	const int h = static_cast<int>(frame->height);

	switch (frame->format) {
	case VIDEO_FORMAT_BGRA: {
		cv::Mat src(h, w, CV_8UC4, frame->data[0], frame->linesize[0]);
		return src.clone();
	}
	case VIDEO_FORMAT_RGBA: {
		cv::Mat src(h, w, CV_8UC4, frame->data[0], frame->linesize[0]);
		cv::Mat dst;
		cv::cvtColor(src, dst, cv::COLOR_RGBA2BGRA);
		return dst;
	}
	case VIDEO_FORMAT_BGR3: {
		cv::Mat src(h, w, CV_8UC3, frame->data[0], frame->linesize[0]);
		cv::Mat dst;
		cv::cvtColor(src, dst, cv::COLOR_BGR2BGRA);
		return dst;
	}
	/* VIDEO_FORMAT_RGB3 not present in this OBS version */
	case VIDEO_FORMAT_YUY2:  // YUYV packed — most common webcam format on Windows
	case VIDEO_FORMAT_YVYU: {
		// Packed YUV 4:2:2 — each "pixel" in the Mat is 2 bytes (Y+UV)
		cv::Mat packed(h, w, CV_8UC2, frame->data[0], frame->linesize[0]);
		cv::Mat bgr;
		const int code = (frame->format == VIDEO_FORMAT_YVYU) ? cv::COLOR_YUV2BGR_YVYU
								      : cv::COLOR_YUV2BGR_YUY2;
		cv::cvtColor(packed, bgr, code);
		cv::Mat bgra;
		cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
		return bgra;
	}
	case VIDEO_FORMAT_UYVY: {
		cv::Mat packed(h, w, CV_8UC2, frame->data[0], frame->linesize[0]);
		cv::Mat bgr;
		cv::cvtColor(packed, bgr, cv::COLOR_YUV2BGR_UYVY);
		cv::Mat bgra;
		cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
		return bgra;
	}
	case VIDEO_FORMAT_BGRX: {
		// 4-byte pixels but alpha is ignored — treat as BGR, add alpha
		cv::Mat src(h, w, CV_8UC4, frame->data[0], frame->linesize[0]);
		cv::Mat bgra = src.clone();
		// Set alpha channel to 255 (BGRX has no meaningful alpha)
		std::vector<cv::Mat> ch;
		cv::split(bgra, ch);
		if (ch.size() == 4) {
			ch[3].setTo(255);
			cv::merge(ch, bgra);
		}
		return bgra;
	}
	case VIDEO_FORMAT_NV12: {
		// Y plane + interleaved UV plane
		cv::Mat yuv(h + h / 2, w, CV_8UC1);
		memcpy(yuv.data, frame->data[0], static_cast<size_t>(h) * frame->linesize[0]);
		memcpy(yuv.data + static_cast<size_t>(h) * w, frame->data[1],
		       static_cast<size_t>(h / 2) * frame->linesize[1]);
		cv::Mat bgr, bgra;
		cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_NV12);
		cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
		return bgra;
	}
	case VIDEO_FORMAT_I420: {
		cv::Mat yuv(h + h / 2, w, CV_8UC1);
		memcpy(yuv.data, frame->data[0], static_cast<size_t>(h) * frame->linesize[0]);
		memcpy(yuv.data + static_cast<size_t>(h) * w, frame->data[1],
		       static_cast<size_t>(h / 4) * frame->linesize[1]);
		memcpy(yuv.data + static_cast<size_t>(h) * w + static_cast<size_t>(h / 4) * w, frame->data[2],
		       static_cast<size_t>(h / 4) * frame->linesize[2]);
		cv::Mat bgr, bgra;
		cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);
		cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);
		return bgra;
	}
	default:
		obs_log(LOG_WARNING, "Bria source: unsupported camera format %d — skipping frame",
			static_cast<int>(frame->format));
		return {};
	}
}

// ---------------------------------------------------------------------------
// Properties — source picker + SSO auth
// ---------------------------------------------------------------------------

static bool bria_source_sign_in_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(data);
	BriaAuthClient::instance().startLoginFlow();

	const BriaAuthClient &auth = BriaAuthClient::instance();
	obs_property_t *status = obs_properties_get(props, "bria_auth_status");
	if (status) {
		obs_property_set_description(status, auth.isCheckingAuth() ? obs_module_text("BriaSigningIn")
									    : obs_module_text("BriaNotSignedIn"));
	}
	obs_property_set_visible(obs_properties_get(props, "btn_sign_in"), false);
	obs_property_set_visible(obs_properties_get(props, "btn_refresh_status"), true);
	obs_property_set_visible(obs_properties_get(props, "btn_sign_out"), false);
	return true;
}

static bool bria_src_auth_update_ui(obs_properties_t *props, obs_property_t * /*p*/, obs_data_t * /*settings*/)
{
	const BriaAuthClient &auth = BriaAuthClient::instance();
	const bool authenticated = auth.isAuthenticated();
	const bool checking = auth.isCheckingAuth();

	obs_property_t *status = obs_properties_get(props, "bria_auth_status");
	if (status) {
		std::string text;
		if (authenticated) {
			text = std::string(obs_module_text("BriaSignedIn")) + " " + auth.getUserEmail();
			if (!auth.getOrgName().empty()) {
				text += " (" + auth.getOrgName() + ")";
			}
		} else if (checking) {
			text = obs_module_text("BriaSigningIn");
		} else {
			text = obs_module_text("BriaNotSignedIn");
		}
		obs_property_set_description(status, text.c_str());
	}

	obs_property_set_visible(obs_properties_get(props, "btn_sign_in"), !authenticated && !checking);
	obs_property_set_visible(obs_properties_get(props, "btn_refresh_status"), checking);
	obs_property_set_visible(obs_properties_get(props, "btn_sign_out"), authenticated);
	return true;
}

static bool bria_source_sign_out_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	auto *s = static_cast<bria_removal_source *>(data);
	BriaAuthClient::instance().logout();
	if (s) {
		std::lock_guard<std::mutex> lock(s->clientMutex);
		if (s->briaClient) {
			s->briaClient->disconnect();
			s->lastConnectedToken.clear();
		}
	}
	return bria_src_auth_update_ui(props, p, nullptr);
}

const char *bria_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BriaCameraSource");
}

obs_properties_t *bria_source_properties(void *data)
{
	auto *s = static_cast<bria_removal_source *>(data);
	obs_properties_t *props = obs_properties_create();

	// Auth section
	obs_properties_add_text(props, "bria_auth_status", obs_module_text("BriaNotSignedIn"), OBS_TEXT_INFO);
	obs_properties_add_button(props, "btn_sign_in", obs_module_text("BriaSignIn"), bria_source_sign_in_clicked);
	obs_properties_add_button(props, "btn_refresh_status", obs_module_text("BriaRefreshStatus"),
				  [](obs_properties_t *p2, obs_property_t *prop, void *d) -> bool {
					  UNUSED_PARAMETER(d);
					  return bria_src_auth_update_ui(p2, prop, nullptr);
				  });
	obs_properties_add_button(props, "btn_sign_out", obs_module_text("BriaSignOut"), bria_source_sign_out_clicked);

	// Camera source selector
	obs_property_t *sources = obs_properties_add_list(props, "camera_source", obs_module_text("BriaCameraInput"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources, obs_module_text("BriaNone"), "");

	// Enumerate async video sources (capture devices, virtual cameras, etc.)
	// Exclude the Bria Camera source itself to avoid self-reference.
	obs_enum_sources(
		[](void *param, obs_source_t *src) -> bool {
			auto *list = static_cast<obs_property_t *>(param);
			const uint32_t flags = obs_source_get_output_flags(src);
			const char *id = obs_source_get_id(src);
			// Only show async video sources (cameras, screen capture, etc.)
			// and exclude the Bria source itself
			if ((flags & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO &&
			    id && strcmp(id, "bria_camera_source") != 0) {
				obs_property_list_add_string(list, obs_source_get_name(src),
							     obs_source_get_name(src));
			}
			return true;
		},
		sources);

	bria_src_auth_update_ui(props, nullptr, nullptr);

	UNUSED_PARAMETER(s);
	return props;
}

void bria_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "camera_source", "");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void *bria_source_create(obs_data_t *settings, obs_source_t *source)
{
	obs_log(LOG_INFO, "Bria source created");
	try {
		auto *s = new bria_removal_source();
		s->source = source;
		s->briaClient = std::make_unique<BriaRmbgClient>();
		bria_source_update(s, settings);
		return s;
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Failed to create Bria source: %s", e.what());
		return nullptr;
	}
}

void bria_source_destroy(void *data)
{
	obs_log(LOG_INFO, "Bria source destroyed");
	auto *s = static_cast<bria_removal_source *>(data);
	if (!s) {
		return;
	}

	if (s->authCallbackRegistered) {
		BriaAuthClient::instance().removeCallback(s->authCallbackHandle);
	}

	if (s->briaClient) {
		s->briaClient->disconnect();
	}

	// Release the active hold on the camera source
	setHeldCameraSource(s, nullptr);

	delete s;
}

void bria_source_activate(void *data)
{
	auto *s = static_cast<bria_removal_source *>(data);
	if (s) {
		s->active = true;
	}
}

void bria_source_deactivate(void *data)
{
	auto *s = static_cast<bria_removal_source *>(data);
	if (s) {
		s->active = false;
	}
}

uint32_t bria_source_getwidth(void *data)
{
	auto *s = static_cast<bria_removal_source *>(data);
	return s ? s->width : 0;
}

uint32_t bria_source_getheight(void *data)
{
	auto *s = static_cast<bria_removal_source *>(data);
	return s ? s->height : 0;
}

void bria_source_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<bria_removal_source *>(data);
	if (!s) {
		return;
	}

	const std::string newCameraName = obs_data_get_string(settings, "camera_source");
	if (newCameraName != s->cameraSourceName) {
		s->cameraSourceName = newCameraName;
		// Acquire the new camera source and force it active so OBS keeps
		// capturing frames even when the source is hidden in the scene.
		obs_source_t *newCam = newCameraName.empty() ? nullptr
							     : obs_get_source_by_name(newCameraName.c_str());
		setHeldCameraSource(s, newCam); // takes ownership of the addref
	}

	if (!s->briaClient) {
		s->briaClient = std::make_unique<BriaRmbgClient>();
	}

	// Mask callback: look up buffered source frame, composite, output to OBS.
	// Both mask and source frame are from the SAME captured moment (frameId match)
	// so edges and person position are always in sync.
	s->briaClient->setMaskCallback([s](cv::Mat foregroundMask, uint64_t frameId) {
		bria_removal_source::StoredFrame stored;
		{
			std::lock_guard<std::mutex> lock(s->frameBufferMutex);
			auto it = s->frameBuffer.find(frameId);
			if (it != s->frameBuffer.end()) {
				stored = std::move(it->second);
				for (auto jt = s->frameBuffer.begin(); jt != s->frameBuffer.end();) {
					if (jt->first <= frameId) {
						jt = s->frameBuffer.erase(jt);
					} else {
						++jt;
					}
				}
			}
		}

		if (stored.bgra.empty()) {
			return; // frame was evicted — skip this mask
		}

		// Resize mask to match the source frame
		if (foregroundMask.size() != stored.bgra.size()) {
			cv::resize(foregroundMask, foregroundMask, stored.bgra.size());
		}

		// Build background mask (Bria: bright = foreground, dark = background)
		const uint8_t threshold = static_cast<uint8_t>(BRIA_SRC_MASK_THRESHOLD * 255.0f);
		cv::Mat bgMask = foregroundMask <= threshold;

		// Zero out background pixels → transparent
		cv::Mat composited = stored.bgra.clone();
		composited.setTo(cv::Scalar(0, 0, 0, 0), bgMask);

		// Output the composited frame. Use the current time as the timestamp
		// so OBS never drops it as "too old" (camera timestamps can be stale
		// relative to the output clock after the Bria processing delay).
		struct obs_source_frame2 outFrame = {};
		outFrame.format = VIDEO_FORMAT_BGRA;
		outFrame.width = static_cast<uint32_t>(composited.cols);
		outFrame.height = static_cast<uint32_t>(composited.rows);
		outFrame.timestamp = os_gettime_ns();
		outFrame.data[0] = composited.data;
		outFrame.linesize[0] = static_cast<uint32_t>(composited.step);
		outFrame.range = VIDEO_RANGE_FULL;

		obs_source_output_video2(s->source, &outFrame);
		obs_log(LOG_DEBUG, "Bria source: output composited frame %llu (%ux%u)",
			static_cast<unsigned long long>(frameId), outFrame.width, outFrame.height);
	});

	// Auth callback: reconnect/disconnect when SSO state changes
	if (!s->authCallbackRegistered) {
		s->authCallbackHandle = BriaAuthClient::instance().addCallback([s]() {
			const std::string token = BriaAuthClient::instance().getApiToken();
			std::thread([s, token]() {
				std::lock_guard<std::mutex> lock(s->clientMutex);
				if (!token.empty() && token != s->lastConnectedToken) {
					s->lastConnectedToken = token;
					s->briaClient->connect(token);
				} else if (token.empty()) {
					s->briaClient->disconnect();
					s->lastConnectedToken.clear();
				}
			}).detach();
		});
		s->authCallbackRegistered = true;
	}

	// Connect immediately if already authenticated
	{
		const std::string token = BriaAuthClient::instance().getApiToken();
		std::lock_guard<std::mutex> lock(s->clientMutex);
		if (!token.empty() && token != s->lastConnectedToken) {
			s->lastConnectedToken = token;
			s->briaClient->connect(token);
		}
	}

	obs_log(LOG_INFO, "Bria source updated, camera: '%s'", s->cameraSourceName.c_str());
}

// ---------------------------------------------------------------------------
// Video tick — capture camera frame, submit to Bria, buffer for mask matching
// ---------------------------------------------------------------------------

void bria_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	auto *s = static_cast<bria_removal_source *>(data);
	if (!s || s->cameraSourceName.empty()) {
		return;
	}

	if (!s->briaClient || !s->briaClient->isConnected()) {
		return;
	}

	// Use the held camera reference (set in bria_source_update).
	// The source is kept active via obs_source_inc_active so it produces
	// frames even when hidden in the scene.
	obs_source_t *camSrc = s->heldCameraSource;
	if (!camSrc) {
		return;
	}

	struct obs_source_frame *frame = obs_source_get_frame(camSrc);
	if (!frame) {
		return;
	}

	// Update width/height so OBS knows the output dimensions
	if (s->width != frame->width || s->height != frame->height) {
		s->width = frame->width;
		s->height = frame->height;
		obs_log(LOG_INFO, "Bria source: camera size %ux%u", s->width, s->height);
	}

	const uint64_t ts = frame->timestamp;
	cv::Mat bgra = obsFrameToBGRA(frame);
	obs_source_release_frame(camSrc, frame);

	if (!bgra.empty()) {
		std::lock_guard<std::mutex> lock(s->clientMutex);
		const uint64_t frameId = s->briaClient->submitFrame(bgra, BRIA_SRC_JPEG_QUALITY);
		if (frameId != UINT64_MAX) {
			std::lock_guard<std::mutex> bufLock(s->frameBufferMutex);
			s->frameBuffer[frameId] = {std::move(bgra), ts};
			while (s->frameBuffer.size() > MAX_BUFFERED_FRAMES) {
				s->frameBuffer.erase(s->frameBuffer.begin());
			}
		}
	}
}
