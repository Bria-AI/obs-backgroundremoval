// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: CC0-1.0

#include "bria-filter.h"

#ifdef _WIN32
#include <wchar.h>
#endif

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <util/base.h>
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

// Case-insensitive substring search — used to match server-provided close
// messages regardless of casing (e.g. "free API calls" vs "free api calls").
static bool containsCaseInsensitive(const std::string &haystack, const std::string &needle)
{
	auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
			      [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
	return it != haystack.end();
}

static constexpr int BRIA_JPEG_QUALITY = 60;

// Once an error popup has been shown for an ongoing error condition, wait
// this long before showing it again (so a stuck reconnect loop reminds the
// user periodically instead of either spamming or going silent).
static constexpr uint64_t BRIA_ERROR_POPUP_REPEAT_MS = 30000;

// ---------------------------------------------------------------------------
// Diagnostics for shader load failures
// ---------------------------------------------------------------------------

// gs_effect_create_from_file() only ever returns NULL on failure — the actual
// compiler/parser error is written by libobs straight to the OBS log via
// blog(). This installs a temporary log handler around the call so that
// error text can be attached to the Sentry report instead of being lost.
// The previous handler is still forwarded to so normal OBS logging is
// unaffected.
struct ShaderLoadLogCtx {
	std::string *out;
	log_handler_t prevHandler;
	void *prevParam;
};

static void bria_shader_load_log_handler(int lvl, const char *msg, va_list args, void *param)
{
	auto *ctx = static_cast<ShaderLoadLogCtx *>(param);

	va_list argsCopy;
	va_copy(argsCopy, args);
	char buf[1024];
	vsnprintf(buf, sizeof(buf), msg, argsCopy);
	va_end(argsCopy);

	if (lvl <= LOG_WARNING) {
		if (!ctx->out->empty())
			ctx->out->append(" | ");
		ctx->out->append(buf);
	}

	if (ctx->prevHandler)
		ctx->prevHandler(lvl, msg, args, ctx->prevParam);
}

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

	// Set once a close code arrives that shouldRetryOnClose() says won't
	// recover on its own (anything but capacity/1013), and only cleared when
	// a fresh connect() is issued (new/changed token, or source
	// reactivation). While set, the connection callback ignores every
	// further Open/Close event on the doomed connection so a reconnect that
	// sneaks in during the disconnect() race below can't overwrite
	// lastCloseCode with some other code (which previously showed e.g.
	// "Connecting… Service Busy" right after a 1008), and can't re-open the
	// popup/API traffic either.
	std::atomic<bool> sessionStoppedPermanently{false};

	// Dedup flag for the subscription-limit popup specifically: it can be detected
	// two independent ways (the WebSocket close message, or BriaAuthClient's own
	// block_reason poll/callback), and whichever gets there first should be the
	// only one to show it. Deliberately separate from sessionStoppedPermanently,
	// which can already be true for an unrelated close (e.g. a prior 1008/1011) —
	// reusing it here would make this flag silently swallow a genuine subscription
	// -limit popup that's unrelated to whatever last stopped the session. Reset
	// alongside sessionStoppedPermanently on every fresh connect.
	std::atomic<bool> subscriptionLimitPopupHandled{false};

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
	obs_properties_add_button2(props, "btn_sign_in", obs_module_text("BriaSignIn"), bria_filter_sign_in_clicked,
				   nullptr);
	obs_properties_add_button2(
		props, "btn_refresh_status", obs_module_text("BriaRefreshStatus"),
		[](obs_properties_t *p2, obs_property_t *prop, void *) -> bool {
			return bria_auth_update_ui(p2, prop, nullptr);
		},
		nullptr);
	obs_properties_add_button2(props, "btn_sign_out", obs_module_text("BriaSignOut"), bria_filter_sign_out_clicked,
				   nullptr);
	obs_properties_add_button2(props, "btn_report_issue", obs_module_text("BriaReportIssue"),
				   bria_filter_report_issue_clicked, nullptr);
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
	// the overlay can show it, and surface a popup — at most one at a time.
	// CapacityExceeded (1013) is the only reason that can recur while the
	// session keeps auto-reconnecting, so it alone is rate-limited to once
	// every BRIA_ERROR_POPUP_REPEAT_MS; every other reason is terminal
	// (session stopped for good) and always shows immediately. When the
	// server sends a detailed error message (e.g. a specific
	// quota-exceeded explanation) it's shown verbatim instead of our
	// generic per-code text. The error state is only
	// cleared once a mask actually arrives (see setMaskCallback below) — a
	// bare reconnect (Open) doesn't prove the underlying issue is resolved,
	// since e.g. an unauthorized session can accept the handshake and then
	// immediately close again on every auto-reconnect attempt.
	tf->briaClient->setConnectionCallback([tf](bool connected, int closeCode, const std::string &closeReason,
						   const std::string &serverMessage) {
		// Once 1008 has permanently failed this connection, ignore
		// everything else it reports — a reconnect that snuck in before
		// disconnect() took effect would otherwise flip the overlay/popup
		// state away from Unauthorized using a close code from a
		// connection we're already tearing down.
		if (tf->sessionStoppedPermanently.load()) {
			return;
		}

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

		BriaCloseReason reason = classifyCloseCode(closeCode);

		// The server can deliver its explanation either as a preceding JSON error
		// frame's "message" (serverMessage) or directly as the WebSocket close
		// reason text (closeReason) — prefer the former, same precedence used for
		// the popup text below, since either channel can carry the free-tier-limit
		// message we're matching on.
		const std::string rawDetail = !serverMessage.empty() ? serverMessage : closeReason;

		// 1008 ("unauthorized") is a generic close — it covers a genuinely invalid/
		// expired token as well as an org blocked for hitting the free-tier call
		// limit, and the message text is the only way to tell them apart (there's
		// no distinct close code for the limit case). When it matches that
		// free-tier-limit text, treat it as the OBS trial ending and show our own
		// popup instead — any other 1008 message (e.g. a real auth failure) is
		// left untouched below.
		const bool isFreeLimitMessage = containsCaseInsensitive(rawDetail, "free api calls");
		const bool isObsTrialLimitClose = reason == BriaCloseReason::Unauthorized && isFreeLimitMessage;
		if (isObsTrialLimitClose) {
			reason = BriaCloseReason::SubscriptionLimitsReached;
			// Mark this handled so the other detection path (BriaAuthClient's
			// block_reason callback, above) knows not to show its own popup too.
			tf->subscriptionLimitPopupHandled.store(true);
		}

		obs_log(LOG_INFO, "Bria removal filter: connection lost (code %d: %s)", closeCode, closeReason.c_str());

		tf->lastCloseCode.store(isObsTrialLimitClose ? kSubscriptionLimitsCloseCode : closeCode);

		// Only capacity-exceeded (1013) recovers by retrying — the server is
		// just full, and a later attempt may land. Every other reason (bad
		// auth, plan/session limit, timeout, or a code we don't recognize)
		// won't recover: the server will just reject every reconnect the
		// same way. ix::WebSocket's automatic reconnection would otherwise
		// keep hammering the API and re-triggering this same callback
		// indefinitely, so tear the connection down for good here; the
		// popup below is the only thing the user sees from this point on.
		// Done on a detached thread since disconnect() does socket I/O and
		// this callback runs on the ixwebsocket thread itself.
		if (!shouldRetryOnClose(reason)) {
			tf->sessionStoppedPermanently.store(true);
			std::thread([tf]() {
				std::lock_guard<std::mutex> lock(tf->clientMutex);
				if (tf->briaClient) {
					tf->briaClient->disconnect();
				}
			}).detach();
		}

		// rawDetail falls back to our generic per-reason text when empty (handled
		// inside bria_show_error_dialog). For an OBS-trial-limit close, ignore the
		// server's text entirely — it's the generic free-tier-limit message, not
		// ours — and let bria_show_error_dialog fall back to
		// BriaErrorSubscriptionLimitsMessage instead.
		const std::string detail = isObsTrialLimitClose ? "" : rawDetail;

		// One popup at a time. CapacityExceeded (1013) can recur many times
		// during a long reconnect loop, so it stays rate-limited to at most
		// once every BRIA_ERROR_POPUP_REPEAT_MS. Every other reason is a
		// terminal, one-shot event — sessionStoppedPermanently (set above)
		// guarantees it can't fire again for this connection — so it must
		// always show immediately rather than risk being swallowed by a
		// cooldown started by an unrelated, still-retrying capacity popup
		// (e.g. a 1008 arriving moments after a 1013 popup was shown).
		const uint64_t nowMs = os_gettime_ns() / 1000000ULL;
		const uint64_t lastShownMs = tf->lastErrorPopupMs.load();
		const bool cooldownElapsed = lastShownMs == 0 || nowMs - lastShownMs >= BRIA_ERROR_POPUP_REPEAT_MS;
		const bool isCapacity = reason == BriaCloseReason::CapacityExceeded;
		const bool mayShow = isCapacity ? cooldownElapsed : true;
		// CapacityExceeded also respects errorPopupInFlight — it can recur many
		// times during a long reconnect loop, and a still-open capacity dialog
		// (dialog.exec() blocks until dismissed) shouldn't get a second one
		// stacked behind it. A terminal reason must not be gated the same way:
		// sessionStoppedPermanently guarantees it fires only once for this
		// connection, so skipping it here because some *other* dialog happens
		// to be open would drop it for good — always queue it regardless.
		const bool blockedByInFlightDialog = isCapacity && tf->errorPopupInFlight.exchange(true);
		// A sign-out already tears the connection down on its own; if that's what
		// produced this close, showing "your trial has ended, upgrade" alongside it
		// is just confusing, not useful.
		const bool suppressForSignOut = isObsTrialLimitClose && BriaAuthClient::instance().isLoggingOut();
		if (mayShow && !blockedByInFlightDialog && !suppressForSignOut) {
			tf->lastErrorPopupMs.store(nowMs);
			QMetaObject::invokeMethod(
				qApp,
				[tf, reason, detail, isCapacity]() {
					// The filter may have been removed from the source
					// while this was queued — tf's shared_ptr keeps the
					// object alive, but there's nothing left for the
					// user to act on, so skip showing it.
					if (!tf->destroyed.load()) {
						bria_show_error_dialog(reason, detail);
					}
					if (isCapacity) {
						tf->errorPopupInFlight.store(false);
					}
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
		tf->authCallbackHandle = BriaAuthClient::instance().addCallback([weakTf = std::weak_ptr<
											 bria_removal_filter>(tf)]() {
			const std::string token = BriaAuthClient::instance().getApiToken();
			const std::string blockReason = BriaAuthClient::instance().getBlockReason();
			const std::string email = BriaAuthClient::instance().getUserEmail();
			if (!email.empty()) {
				BriaAnalytics::instance().identify(email, BriaAuthClient::instance().getUserName(),
								   BriaAuthClient::instance().getOrgId(),
								   BriaAuthClient::instance().getOrgName());
			}

			// The org has hit the OBS trial limits — tear the session down for
			// good and show the same popup used for a server-side WebSocket
			// close, rather than attempting to (re)connect below.
			if (blockReason == BriaAuthClient::BLOCK_REASON_PASSED_SUBSCRIPTION_LIMITS) {
				std::thread([weakTf]() {
					auto lockedTf = weakTf.lock();
					if (!lockedTf) {
						return;
					}
					lockedTf->sessionStoppedPermanently.store(true);
					// This is one of two independent ways the same block can be
					// detected — the other being the WebSocket close handler's
					// own free-tier-limit message check below. exchange(true)
					// makes whichever one gets here first win and the other back
					// off, instead of both queuing their own popup. Deliberately
					// a dedicated flag rather than sessionStoppedPermanently
					// above, which can already be true from an unrelated close.
					if (lockedTf->subscriptionLimitPopupHandled.exchange(true)) {
						return;
					}
					lockedTf->lastCloseCode.store(kSubscriptionLimitsCloseCode);
					{
						std::lock_guard<std::mutex> lock(lockedTf->clientMutex);
						if (lockedTf->briaClient) {
							lockedTf->briaClient->disconnect();
						}
						lockedTf->lastConnectedToken.clear();
					}
					// A deliberate sign-out already clears block state on its own;
					// showing "your trial has ended, upgrade" as someone signs out
					// is just confusing, not useful.
					if (BriaAuthClient::instance().isLoggingOut()) {
						return;
					}
					// Terminal, one-shot event (same reasoning as the WebSocket
					// close handler above) — must not be dropped just because
					// some other dialog (e.g. a still-open capacity popup)
					// currently holds errorPopupInFlight, since nothing will
					// ever re-trigger this once sessionStoppedPermanently is set.
					QMetaObject::invokeMethod(
						qApp,
						[lockedTf]() {
							if (!lockedTf->destroyed.load()) {
								bria_show_error_dialog(
									BriaCloseReason::SubscriptionLimitsReached, "");
							}
						},
						Qt::QueuedConnection);
				}).detach();
				return;
			}

			std::thread([weakTf, token]() {
				auto lockedTf = weakTf.lock();
				if (!lockedTf) {
					return;
				}
				std::lock_guard<std::mutex> lock(lockedTf->clientMutex);
				if (!token.empty() && token != lockedTf->lastConnectedToken) {
					lockedTf->lastConnectedToken = token;
					lockedTf->sessionStoppedPermanently.store(false);
					lockedTf->subscriptionLimitPopupHandled.store(false);
					lockedTf->briaClient->connect(token);
					lockedTf->isDisabled = false;
				} else if (token.empty()) {
					lockedTf->briaClient->disconnect();
					lockedTf->lastConnectedToken.clear();
					// Signing out is a clean slate: without this, re-signing into
					// the *same* still-blocked org afterwards would never re-fire
					// the popup, since the reset above only runs when the token
					// actually changes from lastConnectedToken — but that variable
					// was never set to begin with when the block was detected
					// before ever reaching a successful connect() (see the
					// subscription-limits branch above, which returns early).
					lockedTf->sessionStoppedPermanently.store(false);
					lockedTf->subscriptionLimitPopupHandled.store(false);
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

	std::string shaderLog;
	ShaderLoadLogCtx logCtx{&shaderLog, nullptr, nullptr};
	base_get_log_handler(&logCtx.prevHandler, &logCtx.prevParam);
	base_set_log_handler(bria_shader_load_log_handler, &logCtx);

	// gs_effect_create_from_file()'s error_string out-param carries
	// effect-parser/syntax errors (e.g. malformed .effect text), which are
	// never routed through blog() — only deeper D3D11 HLSL compile failures
	// are. Both must be captured to get the real cause of a load failure.
	char *parserError = nullptr;
	tf->effect = gs_effect_create_from_file(effect_path, &parserError);

	base_set_log_handler(logCtx.prevHandler, logCtx.prevParam);

	if (!tf->effect) {
		const bool fileExists = effect_path && os_file_exists(effect_path);
		const char *deviceName = gs_get_device_name();
		const char *driverVersion = gs_get_driver_version();
		const std::string deviceType = gs_get_device_type() == GS_DEVICE_DIRECT3D_11 ? "D3D11" : "OpenGL";
		if (parserError && *parserError) {
			if (!shaderLog.empty())
				shaderLog.append(" | ");
			shaderLog.append(parserError);
		}
		BriaSentry::captureShaderLoadFailed(effect_path ? effect_path : BRIA_EFFECT_PATH, fileExists, shaderLog,
						    deviceName ? deviceName : "", deviceType,
						    driverVersion ? driverVersion : "");
	}
	bfree(parserError);
	bfree(effect_path);
	obs_leave_graphics();

	// Connect immediately if already authenticated (token restored from config),
	// unless the org already hit the OBS trial limits (block reason known from a
	// prior session's status check, persisted only in-memory so this only matters
	// for a filter re-create within the same OBS run — the auth callback above
	// handles a block discovered afterwards).
	if (BriaAuthClient::instance().getBlockReason() == BriaAuthClient::BLOCK_REASON_PASSED_SUBSCRIPTION_LIMITS) {
		tf->isDisabled = true;
		tf->sessionStoppedPermanently.store(true);
		tf->lastCloseCode.store(kSubscriptionLimitsCloseCode);
		// The overlay alone (driven by lastCloseCode) is easy to miss on a filter
		// that's blocked from the moment it's added/OBS is (re)started — show the
		// popup too, same as the other two detection sites. Dedup'd the same way.
		if (!tf->subscriptionLimitPopupHandled.exchange(true) && !BriaAuthClient::instance().isLoggingOut()) {
			QMetaObject::invokeMethod(
				qApp,
				[tf]() {
					if (!tf->destroyed.load()) {
						bria_show_error_dialog(BriaCloseReason::SubscriptionLimitsReached, "");
					}
				},
				Qt::QueuedConnection);
		}
		return;
	}
	{
		const std::string token = BriaAuthClient::instance().getApiToken();
		std::unique_lock<std::mutex> lock(tf->clientMutex);
		if (!token.empty() && token != tf->lastConnectedToken) {
			tf->lastConnectedToken = token;
			tf->sessionStoppedPermanently.store(false);
			tf->subscriptionLimitPopupHandled.store(false);
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

		// Reconnect on its own thread — connect() does network I/O and must
		// never block the OBS render/UI thread that calls activate/deactivate.
		std::thread([tf]() {
			const std::string token = BriaAuthClient::instance().getApiToken();
			std::lock_guard<std::mutex> lock(tf->clientMutex);
			if (tf->briaClient && !token.empty()) {
				tf->sessionStoppedPermanently.store(false);
				tf->subscriptionLimitPopupHandled.store(false);
				tf->briaClient->connect(token);
				tf->lastConnectedToken = token;
			}
		}).detach();
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

		// Tear down the WebSocket session instead of leaving it open and idle —
		// an idle-but-connected session still occupies a slot against the
		// account's concurrent-session limit (and auto-reconnect on a
		// server-side timeout of that idle session can itself trip the limit),
		// which was surfacing as spurious "4003 session limit reached" closes.
		std::thread([tf]() {
			std::lock_guard<std::mutex> lock(tf->clientMutex);
			if (tf->briaClient) {
				tf->briaClient->disconnect();
			}
		}).detach();
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
// Rules (mirrors shouldRetryOnClose(): only capacity actually retries):
//  - No error, or an unrecognized close code: just the animated
//    "Connecting…" line, as before. Unrecognized also covers our own
//    disconnect()-triggered closes (filter deactivate/destroy), which are
//    not errors.
//  - CapacityExceeded: the only reason still retried automatically, so both
//    lines are shown together — "Connecting…" plus "Service Busy".
//  - Unauthorized / GeneralError / SessionLimitReached / SessionTimeout: the
//    session has been stopped for good (see sessionStoppedPermanently), so
//    no "Connecting…" — just the static reason line (Unauthorized shows
//    none; the popup is the only feedback).
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
		showConnecting = false;
		errorText = "Plan Limit Reached";
		break;
	case BriaCloseReason::CapacityExceeded:
		errorText = "Service Busy";
		break;
	case BriaCloseReason::SessionTimeout:
		showConnecting = false;
		errorText = "Session Timed Out";
		break;
	case BriaCloseReason::SubscriptionLimitsReached:
		showConnecting = false;
		errorText = "Trial Ended - Upgrade to Continue";
		break;
	case BriaCloseReason::Unknown:
	default:
		// Deliberately unchanged: an unrecognized code also covers our own
		// disconnect()-triggered closes (e.g. filter deactivate/destroy),
		// which aren't errors, so keep the plain animated "Connecting…".
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
