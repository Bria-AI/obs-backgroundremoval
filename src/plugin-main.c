/*
 * SPDX-FileCopyrightText: 2021-2026 Roy Shilkrot <roy.shil@gmail.com>
 * SPDX-FileCopyrightText: 2023-2026 Kaito Udagawa <umireon@kaito.tokyo>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include "plugin-support.h"
#include "update-checker/update-checker.h"
#include "bria-welcome-dialog.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info background_removal_filter_info;
extern struct obs_source_info enhance_filter_info;
extern struct obs_source_info bria_filter_info;

// ---------------------------------------------------------------------------
// First-launch welcome dialog
// ---------------------------------------------------------------------------

static void on_obs_frontend_loaded(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	// Only fire once per OBS session.
	obs_frontend_remove_event_callback(on_obs_frontend_loaded, NULL);

	config_t *config = obs_frontend_get_global_config();
	if (!config)
		return;

	const bool already_shown = config_get_bool(config, "BriaPlugin", "WelcomeShown");
	if (!already_shown) {
		config_set_bool(config, "BriaPlugin", "WelcomeShown", true);
		config_save_safe(config, "tmp", NULL);
		bria_show_welcome_dialog();
	}
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

bool obs_module_load(void)
{
	obs_register_source(&background_removal_filter_info);
	obs_register_source(&enhance_filter_info);
	obs_register_source(&bria_filter_info);
	obs_log(LOG_INFO, "Plugin loaded successfully (version %s)", PLUGIN_VERSION);

	obs_frontend_add_event_callback(on_obs_frontend_loaded, NULL);

	check_update();

	return true;
}

void obs_module_unload()
{
	obs_frontend_remove_event_callback(on_obs_frontend_loaded, NULL);
	obs_log(LOG_INFO, "plugin unloaded");
}
