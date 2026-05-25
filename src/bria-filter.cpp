#include "bria-filter.h"

#ifdef _WIN32
#include <wchar.h>
#endif

#include <opencv2/imgproc.hpp>

#include <memory>
#include <mutex>
#include <string>

#include <plugin-support.h>
#include "obs-utils/obs-utils.hpp"
#include "consts.h"
#include "bria-utils/bria-auth-client.hpp"
#include "bria-utils/bria-rmbg-client.hpp"
#include "FilterData.hpp"

// ---------------------------------------------------------------------------
// Internal constants — not exposed as settings for this filter
// ---------------------------------------------------------------------------

static constexpr int BRIA_JPEG_QUALITY = 60;
static constexpr float BRIA_MASK_THRESHOLD = 0.5f;

// ---------------------------------------------------------------------------
// Filter data struct
// ---------------------------------------------------------------------------

struct bria_removal_filter : public filter_data, public std::enable_shared_from_this<bria_removal_filter> {
	bool stopWhenSourceIsInactive = true;

	cv::Mat backgroundMask;

	gs_effect_t *effect = nullptr;

	std::unique_ptr<BriaRmbgClient> briaClient;
	std::string lastConnectedToken;
	BriaAuthClient::CallbackHandle authCallbackHandle{0};
	bool authCallbackRegistered{false};

	cv::Mat pendingForegroundMask;
	std::mutex pendingMaskMutex;
	bool pendingMaskReady = false;

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
	BriaAuthClient::instance().startLoginFlow();
	return bria_auth_update_ui(props, p, nullptr);
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
				  [](obs_properties_t *p2, obs_property_t *prop, void *d) -> bool {
					  return bria_auth_update_ui(p2, prop, nullptr);
				  });
	obs_properties_add_button(props, "btn_sign_out", obs_module_text("BriaSignOut"), bria_filter_sign_out_clicked);

	obs_properties_add_bool(props, "stop_when_source_is_inactive",
				obs_module_text("Stop filter when source is inactive"));

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

	tf->briaClient->setMaskCallback([tf](cv::Mat foregroundMask, uint64_t frameId) {
		UNUSED_PARAMETER(frameId);
		std::lock_guard<std::mutex> lock(tf->pendingMaskMutex);
		tf->pendingForegroundMask = std::move(foregroundMask);
		tf->pendingMaskReady = true;
	});

	// Register auth-change callback so the WebSocket connects/disconnects automatically
	// whenever the user completes (or cancels) the SSO flow.
	// The callback must never block: connect() and disconnect() involve network I/O,
	// so both are done on a detached thread to avoid freezing any caller (including the
	// OBS UI thread during logout).
	if (!tf->authCallbackRegistered) {
		tf->authCallbackHandle = BriaAuthClient::instance().addCallback(
			[weakTf = std::weak_ptr<bria_removal_filter>(tf)]() {
				const std::string token = BriaAuthClient::instance().getApiToken();
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

	obs_enter_graphics();
	char *effect_path = obs_module_file(EFFECT_PATH);
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

	if (tf->backgroundMask.empty()) {
		tf->backgroundMask = cv::Mat(imageBGRA.size(), CV_8UC1, cv::Scalar(255));
	}

	try {
		// Apply any mask received from Bria since last tick
		{
			std::lock_guard<std::mutex> lock(tf->pendingMaskMutex);
			if (tf->pendingMaskReady && !tf->pendingForegroundMask.empty()) {
				cv::Mat foreground = tf->pendingForegroundMask.clone();
				tf->pendingMaskReady = false;

				// Convert grayscale foreground mask → background mask
				// Bria: bright = foreground → we invert for the effect shader
				cv::Mat backgroundMask;
				const uint8_t threshold_value =
					static_cast<uint8_t>(BRIA_MASK_THRESHOLD * 255.0f);
				backgroundMask = foreground <= threshold_value;

				if (backgroundMask.size() != imageBGRA.size()) {
					cv::resize(backgroundMask, backgroundMask, imageBGRA.size());
				}

				std::lock_guard<std::mutex> outLock(tf->outputLock);
				backgroundMask.copyTo(tf->backgroundMask);
			}
		}

		// Submit current frame to Bria
		{
			std::lock_guard<std::mutex> lock(tf->clientMutex);
			tf->briaClient->submitFrame(imageBGRA, BRIA_JPEG_QUALITY);
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "%s", e.what());
	}
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

	gs_texture_t *alphaTexture = nullptr;
	{
		std::lock_guard<std::mutex> lock(tf->outputLock);

		if (tf->backgroundMask.empty()) {
			if (tf->source) {
				obs_source_skip_video_filter(tf->source);
			}
			return;
		}

		alphaTexture = gs_texture_create(tf->backgroundMask.cols, tf->backgroundMask.rows, GS_R8, 1,
						 (const uint8_t **)&tf->backgroundMask.data, 0);

		if (!alphaTexture) {
			obs_log(LOG_ERROR, "Bria removal filter: failed to create alpha texture");
			if (tf->source) {
				obs_source_skip_video_filter(tf->source);
			}
			return;
		}
	}

	if (!obs_source_process_filter_begin(tf->source, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
		if (tf->source) {
			obs_source_skip_video_filter(tf->source);
		}
		gs_texture_destroy(alphaTexture);
		return;
	}

	gs_eparam_t *alphamask = gs_effect_get_param_by_name(tf->effect, "alphamask");
	gs_effect_set_texture(alphamask, alphaTexture);

	gs_blend_state_push();
	gs_reset_blend_state();

	obs_source_process_filter_tech_end(tf->source, tf->effect, 0, 0, "DrawWithoutBlur");

	gs_blend_state_pop();
	gs_texture_destroy(alphaTexture);
}
