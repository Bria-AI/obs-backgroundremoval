/*
 * SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <sentry.h>

#include "plugin-support.h"
#include "update-checker/update-checker.h"
#include "bria-welcome-dialog.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

extern struct obs_source_info bria_filter_info;

static void on_obs_frontend_loaded(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	obs_frontend_remove_event_callback(on_obs_frontend_loaded, NULL);

	config_t *config = obs_frontend_get_app_config();
	if (!config)
		return;

	const bool already_shown = config_get_bool(config, "BriaPlugin", "WelcomeShown");
	if (!already_shown) {
		config_set_bool(config, "BriaPlugin", "WelcomeShown", true);
		config_save_safe(config, "tmp", NULL);
		bria_show_welcome_dialog();
	}
}

bool obs_module_load(void)
{
	char *config_dir = obs_module_config_path(NULL);
	if (config_dir) {
		os_mkdirs(config_dir);
		bfree(config_dir);
	}

	sentry_options_t *sentry_opts = sentry_options_new();
	sentry_options_set_dsn(sentry_opts, BRIA_SENTRY_DSN);
	sentry_options_set_release(sentry_opts, "bria-obs@" PLUGIN_VERSION_STR);
	sentry_options_set_environment(sentry_opts, "production");
	sentry_options_set_enable_metrics(sentry_opts, 1);
	sentry_options_set_traces_sample_rate(sentry_opts, 0.0);
	sentry_options_set_backend(sentry_opts, NULL);
	char *sentry_db = obs_module_config_path("sentry-db");
	if (sentry_db) {
		sentry_options_set_database_path(sentry_opts, sentry_db);
		bfree(sentry_db);
	}
	if (sentry_init(sentry_opts) != 0)
		obs_log(LOG_ERROR, "Sentry init failed");
	else
		obs_log(LOG_INFO, "Sentry initialized");

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
	sentry_close();
}
