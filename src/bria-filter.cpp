// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: CC0-1.0

#include "bria-filter.h"

#ifdef _WIN32
#include <wchar.h>
#endif

#include <opencv2/imgproc.hpp>

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <util/platform.h>
#include <plugin-support.h>
#include "obs-utils/obs-utils.hpp"
#include "consts.h"
#include "bria-utils/bria-auth-client.hpp"
#include "bria-utils/bria-rmbg-client.hpp"
#include "FilterData.hpp"
#include "bria-analytics.hpp"

#include <QDesktopServices>
#include <QUrl>

// ---------------------------------------------------------------------------
// Internal constants — not exposed as settings for this filter
// ---------------------------------------------------------------------------

static constexpr int BRIA_JPEG_QUALITY = 60;

// ---------------------------------------------------------------------------
// Filter data struct
// ---------------------------------------------------------------------------

struct bria_removal_filter : public filter_data, public std::enable_shared_from_this<bria_removal_filter> {
	bool stopWhenSourceIsInactive = true;

	gs_effect_t *effect = nullptr;

	std::unique_ptr<BriaRmbgClient> briaClient;
	std::string lastConnectedToken;
	BriaAuthClient::CallbackHandle authCallbackHandle{0};
	bool authCallbackRegistered{false};

	// Frame buffer: frameId → BGRA pixels captured at submission time.
	// The mask callback looks up the matching frame so compositing is always
	// temporally synchronised (same approach as bria-source.cpp).
	std::unordered_map<uint64_t, cv::Mat> frameBuffer;
	std::mutex frameBufferMutex;
	static constexpr size_t MAX_BUFFERED_FRAMES = 32;

	// CPU-composited BGRA output (background zeroed out).  Updated by the
	// mask callback; read by video_render under outputLock.
	cv::Mat compositedBGRA;

	std::mutex clientMutex;

	~bria_removal_filter() { obs_log(LOG_INFO, "Bria removal filter destructor called"); }
};

// ---------------------------------------------------------------------------
// Name
// ---------------------------------------------------------------------------

const char *bria_filter_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BriaRemoveBackground");
}

// ---------------------------------------------------------------------------
// Properties — SSO auth controls only
// ---------------------------------------------------------------------------

// Updates status text (via obs_property_set_description) and button visibility directly
// on the live obs_properties_t object.  Called both on initial display and from every
// button callback so that OBS sees the new state before it processes the return value.
static bool bria_auth_update_ui(obs_properties_t *props, obs_property_t * /*p*/, obs_data_t * /*settings*/)
{
	const BriaAuthClient &auth = BriaAuthClient::instance();
	const bool authenticated = auth.isAuthenticated();
	const bool checking = auth.isCheckingAuth();

	// OBS_TEXT_INFO displays obs_property_description(), not the settings value.
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

static bool bria_filter_sign_in_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(data);
	BriaAnalytics::instance().capture("obs_plugin_sign_in_clicked");
	BriaAuthClient::instance().startLoginFlow();
	return bria_auth_update_ui(props, p, nullptr);
}

static bool bria_filter_report_issue_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	BriaAnalytics::instance().capture("obs_plugin_report_issue_clicked");
	QDesktopServices::openUrl(QUrl("https://github.com/Bria-AI/obs-backgroundremoval/issues"));
	return false;
}

static bool bria_filter_sign_out_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(data);
	BriaAuthClient::instance().logout();
	return bria_auth_update_ui(props, p, nullptr);
}

obs_properties_t *bria_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	// Status text — description updated live by bria_auth_update_ui
	obs_properties_add_text(props, "bria_auth_status", obs_module_text("BriaNotSignedIn"), OBS_TEXT_INFO);

	// All auth buttons always present; visibility controlled by bria_auth_update_ui
	obs_properties_add_button(props, "btn_sign_in", obs_module_text("BriaSignIn"), bria_filter_sign_in_clicked);
	obs_properties_add_button(props, "btn_refresh_status", obs_module_text("BriaRefreshStatus"),
				  [](obs_properties_t *p2, obs_property_t *prop, void *) -> bool {
					  return bria_auth_update_ui(p2, prop, nullptr);
				  });
	obs_properties_add_button(props, "btn_sign_out", obs_module_text("BriaSignOut"), bria_filter_sign_out_clicked);
	obs_properties_add_button(props, "btn_report_issue", obs_module_text("BriaReportIssue"),
				  bria_filter_report_issue_clicked);
	obs_properties_add_bool(props, "stop_when_source_is_inactive", obs_module_text("BriaStopWhenInactive"));

	// Set initial state (text + visibility) when the dialog first opens
	bria_auth_update_ui(props, nullptr, nullptr);

	UNUSED_PARAMETER(data);
	return props;
}

void bria_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "stop_when_source_is_inactive", true);
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------

void bria_filter_update(void *data, obs_data_t *settings)
{
	obs_log(LOG_INFO, "Bria removal filter updated");

	auto *ptr = static_cast<std::shared_ptr<bria_removal_filter> *>(data);
	if (!ptr) {
		return;
	}

	std::shared_ptr<bria_removal_filter> tf = *ptr;
	if (!tf) {
		return;
	}

	tf->isDisabled = true;
	tf->stopWhenSourceIsInactive = obs_data_get_bool(settings, "stop_when_source_is_inactive");

	if (!tf->briaClient) {
		tf->briaClient = std::make_unique<BriaRmbgClient>();
	}

	// When the WebSocket drops (e.g. 1013 capacity-exceeded) clear the
	// composited output and the frame buffer so the "Connecting…" overlay
	// reappears immediately.  On reconnect the overlay disappears as soon as
	// the first mask arrives — no extra action needed.
	tf->briaClient->setConnectionCallback([tf](bool connected) {
		if (!connected) {
			{
				std::lock_guard<std::mutex> outLock(tf->outputLock);
				tf->compositedBGRA.release();
			}
			{
				std::lock_guard<std::mutex> bufLock(tf->frameBufferMutex);
				tf->frameBuffer.clear();
			}
			obs_log(LOG_INFO, "Bria removal filter: connection lost — showing Connecting overlay");
		} else {
			obs_log(LOG_INFO, "Bria removal filter: reconnected — resuming background removal");
		}
	});

	tf->briaClient->setMaskCallback([tf](cv::Mat foregroundMask, uint64_t frameId) {
		// Retrieve the source frame that was submitted with this frameId.
		cv::Mat matchedBGRA;
		{
			std::lock_guard<std::mutex> bufLock(tf->frameBufferMutex);
			auto it = tf->frameBuffer.find(frameId);
			if (it != tf->frameBuffer.end()) {
				matchedBGRA = std::move(it->second);
				// Evict this frame and all older ones — they are no longer needed.
				for (auto jt = tf->frameBuffer.begin(); jt != tf->frameBuffer.end();) {
					if (jt->first <= frameId)
						jt = tf->frameBuffer.erase(jt);
					else
						++jt;
				}
			}
		}

		if (matchedBGRA.empty())
			return; // frame was evicted (buffer overflow) — skip this mask

		// Resize mask to match the captured frame dimensions.
		if (foregroundMask.size() != matchedBGRA.size())
			cv::resize(foregroundMask, foregroundMask, matchedBGRA.size());

		// Build a one-shot LUT that applies an S-curve to the mask:
		//  1. Values <= FRINGE_CUT  → 0   : kills JPEG-ringing white fringe at edges
		//  2. Values above cut      → gamma-boosted toward 255 : opaque interior,
		//                             soft hair strands preserved
		//
		// Example mappings (cut=20, gamma=0.5):
		//   raw  10 (JPEG ringing)     →   0  (fringe removed)
		//   raw  50 (wispy hair)       →  87  (semi-transparent)
		//   raw 128 (hair edge)        → 173  (soft edge)
		//   raw 200 (uncertain head)   → 223  (nearly opaque)
		//   raw 255 (solid foreground) → 255  (fully opaque)
		static constexpr int FRINGE_CUT = 20;
		static constexpr float ALPHA_GAMMA = 0.5f;
		static cv::Mat lut; // computed once, reused every frame
		if (lut.empty()) {
			lut = cv::Mat(1, 256, CV_8U);
			uchar *p = lut.data;
			for (int i = 0; i < 256; ++i) {
				if (i <= FRINGE_CUT) {
					p[i] = 0;
				} else {
					const float v = static_cast<float>(i - FRINGE_CUT) /
							static_cast<float>(255 - FRINGE_CUT);
					p[i] = cv::saturate_cast<uchar>(std::pow(v, ALPHA_GAMMA) * 255.0f);
				}
			}
		}
		cv::Mat softMask;
		cv::LUT(foregroundMask, lut, softMask);

		cv::Mat composited = matchedBGRA.clone();
		std::vector<cv::Mat> channels;
		cv::split(composited, channels); // [B, G, R, A]
		channels[3] = softMask;          // replace A with S-curve mask
		cv::merge(channels, composited);

		std::lock_guard<std::mutex> outLock(tf->outputLock);
		tf->compositedBGRA = std::move(composited);
	});

	// Register auth-change callback so the WebSocket connects/disconnects automatically
	// whenever the user completes (or cancels) the SSO flow.
	// The callback must never block: connect() and disconnect() involve network I/O,
	// so both are done on a detached thread to avoid freezing any caller (including the
	// OBS UI thread during logout).
	if (!tf->authCallbackRegistered) {
		tf->authCallbackHandle =
			BriaAuthClient::instance().addCallback([weakTf = std::weak_ptr<bria_removal_filter>(tf)]() {
				const std::string token = BriaAuthClient::instance().getApiToken();
				const std::string email = BriaAuthClient::instance().getUserEmail();
				if (!email.empty()) {
					BriaAnalytics::instance().identify(email,
									   BriaAuthClient::instance().getUserName(),
									   BriaAuthClient::instance().getOrgId(),
									   BriaAuthClient::instance().getOrgName());
				}
				std::thread([weakTf, token]() {
					auto lockedTf = weakTf.lock();
					if (!lockedTf) {
						return;
					}
					std::lock_guard<std::mutex> lock(lockedTf->clientMutex);
					if (!token.empty() && token != lockedTf->lastConnectedToken) {
						lockedTf->lastConnectedToken = token;
						lockedTf->briaClient->connect(token);
						lockedTf->isDisabled = false;
					} else if (token.empty()) {
						lockedTf->briaClient->disconnect();
						lockedTf->lastConnectedToken.clear();
					}
				}).detach();
			});
		tf->authCallbackRegistered = true;
	}

	// For sessions restored from config (OBS restart), the auth callback
	// won't fire again — identify here so PostHog gets the user identity.
	{
		const std::string email = BriaAuthClient::instance().getUserEmail();
		if (!email.empty()) {
			BriaAnalytics::instance().identify(email, BriaAuthClient::instance().getUserName(),
							   BriaAuthClient::instance().getOrgId(),
							   BriaAuthClient::instance().getOrgName());
		}
	}

	obs_enter_graphics();
	char *effect_path = obs_module_file(BRIA_EFFECT_PATH);
	gs_effect_destroy(tf->effect);
	tf->effect = gs_effect_create_from_file(effect_path, NULL);
	bfree(effect_path);
	obs_leave_graphics();

	// Connect immediately if already authenticated (token restored from config)
	{
		const std::string token = BriaAuthClient::instance().getApiToken();
		std::unique_lock<std::mutex> lock(tf->clientMutex);
		if (!token.empty() && token != tf->lastConnectedToken) {
			tf->lastConnectedToken = token;
			if (!tf->briaClient->connect(token)) {
				obs_log(LOG_ERROR, "Bria removal filter: failed to connect to API");
			}
		} else if (token.empty()) {
			obs_log(LOG_INFO, "Bria removal filter: not signed in");
			tf->isDisabled = true;
			return;
		}
	}

	tf->isDisabled = false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void bria_filter_activate(void *data)
{
	auto *ptr = static_cast<std::shared_ptr<bria_removal_filter> *>(data);
	if (!ptr) {
		return;
	}
	std::shared_ptr<bria_removal_filter> tf = *ptr;
	if (tf && tf->stopWhenSourceIsInactive) {
		obs_log(LOG_INFO, "Bria removal filter activated");
		tf->isDisabled = false;
	}
}

void bria_filter_deactivate(void *data)
{
	auto *ptr = static_cast<std::shared_ptr<bria_removal_filter> *>(data);
	if (!ptr) {
		return;
	}
	std::shared_ptr<bria_removal_filter> tf = *ptr;
	if (tf && tf->stopWhenSourceIsInactive) {
		obs_log(LOG_INFO, "Bria removal filter deactivated");
		tf->isDisabled = true;
	}
}

void *bria_filter_create(obs_data_t *settings, obs_source_t *source)
{
	obs_log(LOG_INFO, "Bria removal filter created");
	BriaAnalytics::instance().capture("obs_plugin_filter_added");
	try {
		auto instance = std::make_shared<bria_removal_filter>();

		instance->source = source;
		instance->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
		instance->briaClient = std::make_unique<BriaRmbgClient>();

		auto ptr = new std::shared_ptr<bria_removal_filter>(instance);
		bria_filter_update(ptr, settings);

		return ptr;
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "Failed to create Bria removal filter: %s", e.what());
		return nullptr;
	}
}

void bria_filter_destroy(void *data)
{
	obs_log(LOG_INFO, "Bria removal filter destroyed");

	auto *ptr = static_cast<std::shared_ptr<bria_removal_filter> *>(data);
	if (ptr) {
		if (*ptr) {
			(*ptr)->isDisabled = true;

			if ((*ptr)->authCallbackRegistered) {
				BriaAuthClient::instance().removeCallback((*ptr)->authCallbackHandle);
				(*ptr)->authCallbackRegistered = false;
			}

			if ((*ptr)->briaClient) {
				(*ptr)->briaClient->disconnect();
			}

			obs_enter_graphics();
			gs_texrender_destroy((*ptr)->texrender);
			if ((*ptr)->stagesurface) {
				gs_stagesurface_destroy((*ptr)->stagesurface);
			}
			gs_effect_destroy((*ptr)->effect);
			obs_leave_graphics();
		}
		delete ptr;
	}
}

// ---------------------------------------------------------------------------
// Video pipeline
// ---------------------------------------------------------------------------

void bria_filter_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	auto *ptr = static_cast<std::shared_ptr<bria_removal_filter> *>(data);
	if (!ptr) {
		return;
	}

	std::shared_ptr<bria_removal_filter> tf = *ptr;

	if (!tf || tf->isDisabled) {
		return;
	}

	if (!obs_source_enabled(tf->source)) {
		return;
	}

	if (!tf->briaClient) {
		return;
	}

	cv::Mat imageBGRA;
	{
		std::unique_lock<std::mutex> lock(tf->inputBGRALock, std::try_to_lock);
		if (!lock.owns_lock() || tf->inputBGRA.empty()) {
			return;
		}
		imageBGRA = tf->inputBGRA.clone();
	}

	try {
		// Submit current frame to Bria and buffer it so the mask callback can
		// retrieve the exact pixels that were sent (frameId-matched compositing).
		std::lock_guard<std::mutex> clientLock(tf->clientMutex);
		const uint64_t frameId = tf->briaClient->submitFrame(imageBGRA, BRIA_JPEG_QUALITY);
		if (frameId != UINT64_MAX) {
			std::lock_guard<std::mutex> bufLock(tf->frameBufferMutex);
			tf->frameBuffer[frameId] = std::move(imageBGRA);
			while (tf->frameBuffer.size() > bria_removal_filter::MAX_BUFFERED_FRAMES)
				tf->frameBuffer.erase(tf->frameBuffer.begin());
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "%s", e.what());
	}
}

// ---------------------------------------------------------------------------
// "Connecting…" overlay helper
// ---------------------------------------------------------------------------

// Blends the "Connecting…" banner onto the live camera frame so users get
// clear feedback while the plugin is waiting for the first mask from the API.
// The number of trailing dots pulses over time so the indicator feels alive.
static cv::Mat makeConnectingFrame(const cv::Mat &bgra)
{
	cv::Mat frame = bgra.clone();
	if (frame.empty())
		return frame;

	const int w = frame.cols;
	const int h = frame.rows;

	// Animate the trailing dots: "Connecting." → ".." → "..." every 600 ms
	const uint64_t nowMs = os_gettime_ns() / 1000000ULL;
	const int dots = static_cast<int>((nowMs / 600ULL) % 3) + 1;
	const std::string text = "Connecting" + std::string(dots, '.');

	const int font = cv::FONT_HERSHEY_SIMPLEX;
	const double fontScale = std::max(0.9, w / 1280.0 * 1.4);
	const int thickness = std::max(2, static_cast<int>(fontScale * 1.5));
	int baseline = 0;
	const cv::Size textSz = cv::getTextSize(text, font, fontScale, thickness, &baseline);

	// Semi-transparent dark banner centred vertically
	const int bannerH = textSz.height * 3;
	const int bannerY = (h - bannerH) / 2;
	const cv::Rect bannerRect(0, bannerY, w, bannerH);
	cv::Mat roi = frame(bannerRect);
	cv::Mat dark(roi.size(), CV_8UC4, cv::Scalar(0, 0, 0, 255));
	cv::addWeighted(roi, 0.35, dark, 0.65, 0.0, roi);

	// Centre text within the banner
	const cv::Point textOrg((w - textSz.width) / 2, bannerY + (bannerH + textSz.height) / 2 - baseline);

	// Drop shadow for readability on any background
	cv::putText(frame, text, textOrg + cv::Point(2, 2), font, fontScale, cv::Scalar(0, 0, 0, 255), thickness + 2,
		    cv::LINE_AA);
	// White text
	cv::putText(frame, text, textOrg, font, fontScale, cv::Scalar(255, 255, 255, 255), thickness, cv::LINE_AA);

	return frame;
}

void bria_filter_video_render(void *data, gs_effect_t *_effect)
{
	UNUSED_PARAMETER(_effect);

	auto *ptr = static_cast<std::shared_ptr<bria_removal_filter> *>(data);
	if (!ptr) {
		return;
	}

	std::shared_ptr<bria_removal_filter> tf = *ptr;

	if (!tf || tf->isDisabled) {
		if (tf && tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		return;
	}

	uint32_t width, height;
	if (!getRGBAFromStageSurface(tf.get(), width, height)) {
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		return;
	}

	if (!tf->effect) {
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		return;
	}

	// Grab the latest CPU-composited frame (frameId-matched, always in sync).
	// Before the first mask arrives, show the live camera feed with a
	// "Connecting…" overlay so users know the plugin is working.
	cv::Mat composited;
	{
		std::lock_guard<std::mutex> lock(tf->outputLock);
		if (!tf->compositedBGRA.empty())
			composited = tf->compositedBGRA.clone();
	}

	if (composited.empty()) {
		cv::Mat upstream;
		{
			std::unique_lock<std::mutex> upLock(tf->inputBGRALock, std::try_to_lock);
			if (upLock.owns_lock() && !tf->inputBGRA.empty())
				upstream = tf->inputBGRA.clone();
		}
		if (upstream.empty()) {
			obs_source_skip_video_filter(tf->source);
			return;
		}
		composited = makeConnectingFrame(upstream);
	}

	// Upload CPU-composited BGRA to a temporary GPU texture.
	gs_texture_t *compTex = gs_texture_create(static_cast<uint32_t>(composited.cols),
						  static_cast<uint32_t>(composited.rows), GS_BGRA, 1,
						  (const uint8_t **)&composited.data, 0);

	if (!compTex) {
		obs_log(LOG_ERROR, "Bria removal filter: failed to create composited texture");
		obs_source_skip_video_filter(tf->source);
		return;
	}

	if (!obs_source_process_filter_begin(tf->source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
		gs_texture_destroy(compTex);
		obs_source_skip_video_filter(tf->source);
		return;
	}

	gs_eparam_t *precomp = gs_effect_get_param_by_name(tf->effect, "precomposited");
	gs_effect_set_texture(precomp, compTex);

	gs_blend_state_push();
	gs_reset_blend_state();

	obs_source_process_filter_tech_end(tf->source, tf->effect, 0, 0, "DrawPrecomposited");

	gs_blend_state_pop();
	gs_texture_destroy(compTex);
}
