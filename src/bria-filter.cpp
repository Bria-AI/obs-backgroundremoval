// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: CC0-1.0

#include "bria-filter.h"

#ifdef _WIN32
#include <wchar.h>
#endif

#include <opencv2/imgproc.hpp>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <util/platform.h>
#include <plugin-support.h>
#include "obs-utils/obs-utils.hpp"
#include "consts.h"
#include "bria-utils/bria-auth-client.hpp"
#include "bria-utils/bria-rmbg-client.hpp"
#include "FilterData.hpp"
#include "bria-analytics.hpp"
#include "bria-utils/bria-sentry.hpp"
#include "bria-error-dialog.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QMetaObject>
#include <QUrl>

// ---------------------------------------------------------------------------
// Internal constants — not exposed as settings for this filter
// ---------------------------------------------------------------------------

static constexpr int BRIA_JPEG_QUALITY = 60;

// Once an error popup has been shown for an ongoing error condition, wait
// this long before showing it again (so a stuck reconnect loop reminds the
// user periodically instead of either spamming or going silent).
static constexpr uint64_t BRIA_ERROR_POPUP_REPEAT_MS = 30000;

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

	// Last known WebSocket close code (0 = none/connected). Drives the overlay
	// text, the Filter properties error status, and which error popup to show.
	std::atomic<int> lastCloseCode{0};
	// os_gettime_ns()/1e6 timestamp of the last time an error popup was shown,
	// across ALL error types; 0 means none shown yet. Enforces a single
	// global cooldown so switching between error codes can't chain popups.
	std::atomic<uint64_t> lastErrorPopupMs{0};
	// True while a popup is queued/being displayed on the Qt thread — ensures
	// only one is ever in flight at a time.
	std::atomic<bool> errorPopupInFlight{false};
	// Set once in bria_filter_destroy(). A popup can already be queued on the
	// Qt thread (via QMetaObject::invokeMethod) when the filter is removed —
	// the queued lambda keeps this object alive via its captured shared_ptr,
	// so it still runs later; this flag lets it detect that and skip showing
	// the dialog for a filter that no longer exists.
	std::atomic<bool> destroyed{false};

	// Frame buffer: frameId → BGRA pixels captured at submission time.
	// The mask callback looks up the matching frame so compositing is always
	// temporally synchronised (same approach as bria-source.cpp).
	std::unordered_map<uint64_t, cv::Mat> frameBuffer;
	std::mutex frameBufferMutex;
	static constexpr size_t MAX_BUFFERED_FRAMES = 32;

	// used for FPS tracking
	uint64_t fpsWindowStartNs{0};
	uint64_t fpsWindowSubmitted{0};
	uint64_t fpsWindowDropped{0};
	static constexpr uint64_t FPS_REPORT_INTERVAL_NS = 60ULL * 1000000000ULL;

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

	// When the WebSocket drops clear the composited output and the frame
	// buffer so the "Connecting…"/error overlay reappears immediately.  For
	// known close codes (1008/1011/4003/1013/4008) also record the reason so
	// the overlay can show it, and surface a popup — at most one at a time,
	// and at most once every BRIA_ERROR_POPUP_REPEAT_MS overall (not per
	// error code), so a stuck reconnect loop — even one that flaps between
	// different error codes — can't stack popups, while still periodically
	// reminding the user it's unresolved. When the server sends a detailed
	// error message (e.g. a specific quota-exceeded explanation) it's shown
	// verbatim instead of our generic per-code text. The error state is only
	// cleared once a mask actually arrives (see setMaskCallback below) — a
	// bare reconnect (Open) doesn't prove the underlying issue is resolved,
	// since e.g. an unauthorized session can accept the handshake and then
	// immediately close again on every auto-reconnect attempt.
	tf->briaClient->setConnectionCallback([tf](bool connected, int closeCode, const std::string &closeReason,
						    const std::string &serverMessage) {
		if (connected) {
			obs_log(LOG_INFO, "Bria removal filter: reconnected — waiting for first mask");
			return;
		}

		{
			std::lock_guard<std::mutex> outLock(tf->outputLock);
			tf->compositedBGRA.release();
		}
		{
			std::lock_guard<std::mutex> bufLock(tf->frameBufferMutex);
			tf->frameBuffer.clear();
		}

		const BriaCloseReason reason = classifyCloseCode(closeCode);
		if (reason == BriaCloseReason::Unknown) {
			tf->lastCloseCode.store(0);
			tf->lastErrorPopupMs.store(0);
			obs_log(LOG_INFO, "Bria removal filter: connection lost (code %d) — showing Connecting overlay",
				closeCode);
			return;
		}

		obs_log(LOG_INFO, "Bria removal filter: connection lost (code %d: %s) — showing error overlay",
			closeCode, closeReason.c_str());

		tf->lastCloseCode.store(closeCode);

		// Prefer a detailed message from a preceding JSON error frame; the
		// WebSocket close frame's own reason text is often the actual
		// server-provided explanation too (e.g. a specific quota-exceeded
		// message), so fall back to that before finally falling back to our
		// generic per-reason text (handled inside bria_show_error_dialog).
		const std::string detail = !serverMessage.empty() ? serverMessage : closeReason;

		// One popup at a time, at least BRIA_ERROR_POPUP_REPEAT_MS apart —
		// regardless of whether the error code just changed, so flapping
		// between e.g. capacity-exceeded and unauthorized can't chain popups.
		const uint64_t nowMs = os_gettime_ns() / 1000000ULL;
		const uint64_t lastShownMs = tf->lastErrorPopupMs.load();
		const bool cooldownElapsed = lastShownMs == 0 || nowMs - lastShownMs >= BRIA_ERROR_POPUP_REPEAT_MS;
		if (cooldownElapsed && !tf->errorPopupInFlight.exchange(true)) {
			tf->lastErrorPopupMs.store(nowMs);
			QMetaObject::invokeMethod(
				qApp,
				[tf, reason, detail]() {
					// The filter may have been removed from the source
					// while this was queued — tf's shared_ptr keeps the
					// object alive, but there's nothing left for the
					// user to act on, so skip showing it.
					if (!tf->destroyed.load()) {
						bria_show_error_dialog(reason, detail);
					}
					tf->errorPopupInFlight.store(false);
				},
				Qt::QueuedConnection);
		}
	});

	tf->briaClient->setMaskCallback([tf](cv::Mat foregroundMask, uint64_t frameId) {
		// A mask actually arriving proves the connection is genuinely healthy
		// again — this is the only place that clears the error state (see the
		// connection callback above for why Open alone isn't enough).
		tf->lastCloseCode.store(0);
		tf->lastErrorPopupMs.store(0);

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
	if (!tf->effect)
		BriaSentry::captureShaderLoadFailed(effect_path ? effect_path : BRIA_EFFECT_PATH);
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
		BriaSentry::captureException("filter_create", e.what());
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
			(*ptr)->destroyed.store(true);

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
			tf->fpsWindowSubmitted++;
		} else {
			tf->fpsWindowDropped++;
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "%s", e.what());
		BriaSentry::captureException("video_tick", e.what());
	}

	// FPS report every 60 s
	const uint64_t nowNs = os_gettime_ns();
	if (tf->fpsWindowStartNs == 0)
		tf->fpsWindowStartNs = nowNs;
	if (nowNs - tf->fpsWindowStartNs >= bria_removal_filter::FPS_REPORT_INTERVAL_NS) {
		const double windowSecs = static_cast<double>(nowNs - tf->fpsWindowStartNs) / 1e9;
		const double submittedFps = static_cast<double>(tf->fpsWindowSubmitted) / windowSecs;
		BriaSentry::captureFpsReport(submittedFps, tf->fpsWindowSubmitted, tf->fpsWindowDropped);
		tf->fpsWindowStartNs = nowNs;
		tf->fpsWindowSubmitted = 0;
		tf->fpsWindowDropped = 0;
	}
}

// ---------------------------------------------------------------------------
// "Connecting…" overlay helper
// ---------------------------------------------------------------------------

// Blends a status banner onto the live camera frame so users get clear
// feedback while the plugin is waiting for the first mask from the API, or
// when the last WebSocket close code indicates a known error condition.
//
// Rules:
//  - No error (or an unrecognized close code): just the animated
//    "Connecting…" line, as before.
//  - Unauthorized / GeneralError: these won't recover on their own (need
//    sign-in, or are a server-side fault), so only the error line is shown —
//    no "Connecting…" underneath it.
//  - SessionLimitReached / CapacityExceeded / SessionTimeout: these are all
//    still retried automatically, so both lines are shown together —
//    "Connecting…" plus the specific reason.
static cv::Mat makeConnectingFrame(const cv::Mat &bgra, int closeCode)
{
	cv::Mat frame = bgra.clone();
	if (frame.empty())
		return frame;

	const int w = frame.cols;
	const int h = frame.rows;

	// Animate the trailing dots: "Connecting." → ".." → "..." every 600 ms
	const uint64_t nowMs = os_gettime_ns() / 1000000ULL;
	const int dots = static_cast<int>((nowMs / 600ULL) % 3) + 1;
	const std::string connectingText = "Connecting" + std::string(dots, '.');

	bool showConnecting = true;
	std::string errorText;
	switch (classifyCloseCode(closeCode)) {
	case BriaCloseReason::Unauthorized:
		showConnecting = false;
		errorText = "";
		break;
	case BriaCloseReason::GeneralError:
		showConnecting = false;
		errorText = "Something Went Wrong";
		break;
	case BriaCloseReason::SessionLimitReached:
		errorText = "Plan Limit Reached";
		break;
	case BriaCloseReason::CapacityExceeded:
		errorText = "Service Busy";
		break;
	case BriaCloseReason::SessionTimeout:
		errorText = "Session Timed Out";
		break;
	case BriaCloseReason::Unknown:
	default:
		break;
	}

	std::vector<std::string> lines;
	if (showConnecting)
		lines.push_back(connectingText);
	if (!errorText.empty())
		lines.push_back(errorText);

	const int font = cv::FONT_HERSHEY_SIMPLEX;
	const double fontScale = std::max(0.9, w / 1280.0 * 1.4);
	const int thickness = std::max(2, static_cast<int>(fontScale * 1.5));

	int maxTextWidth = 0;
	int lineHeight = 0;
	int baseline = 0;
	for (const std::string &line : lines) {
		const cv::Size textSz = cv::getTextSize(line, font, fontScale, thickness, &baseline);
		maxTextWidth = std::max(maxTextWidth, textSz.width);
		lineHeight = std::max(lineHeight, textSz.height + baseline);
	}
	const int lineSpacing = lineHeight / 2;
	const int textBlockHeight =
		static_cast<int>(lines.size()) * lineHeight + (static_cast<int>(lines.size()) - 1) * lineSpacing;

	// Semi-transparent dark banner centred vertically, sized to fit all lines
	const int bannerH = textBlockHeight + lineHeight * 2;
	const int bannerY = (h - bannerH) / 2;
	const cv::Rect bannerRect(0, bannerY, w, bannerH);
	cv::Mat roi = frame(bannerRect);
	cv::Mat dark(roi.size(), CV_8UC4, cv::Scalar(0, 0, 0, 255));
	cv::addWeighted(roi, 0.35, dark, 0.65, 0.0, roi);

	// Draw each line, stacked and centred within the banner
	int y = bannerY + (bannerH - textBlockHeight) / 2 + lineHeight - baseline;
	for (const std::string &line : lines) {
		const cv::Size textSz = cv::getTextSize(line, font, fontScale, thickness, &baseline);
		const cv::Point textOrg((w - textSz.width) / 2, y);

		// Drop shadow for readability on any background
		cv::putText(frame, line, textOrg + cv::Point(2, 2), font, fontScale, cv::Scalar(0, 0, 0, 255),
			    thickness + 2, cv::LINE_AA);
		// White text
		cv::putText(frame, line, textOrg, font, fontScale, cv::Scalar(255, 255, 255, 255), thickness,
			    cv::LINE_AA);

		y += lineHeight + lineSpacing;
	}

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
		composited = makeConnectingFrame(upstream, tf->lastCloseCode.load());
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
