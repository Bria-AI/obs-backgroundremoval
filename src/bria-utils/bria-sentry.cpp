// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: CC0-1.0

#include "bria-sentry.hpp"
#include "bria-auth-client.hpp"

#include <sentry.h>

#include <cstdint>
#include <string>

namespace BriaSentry {

void setUser(const std::string &orgId)
{
	sentry_value_t user = sentry_value_new_object();
	sentry_value_set_by_key(user, "id", sentry_value_new_string(orgId.c_str()));
	sentry_set_user(user);
}

void clearUser()
{
	sentry_remove_user();
}

void captureWsDisconnect(int code, const std::string &reason, int reconnectCount)
{
	if (code == 1000)
		return;

	const std::string codeStr = std::to_string(code);
	const std::string msg = "WebSocket closed: code=" + codeStr + " " + reason;
	sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_WARNING, "ws", msg.c_str());

	sentry_value_t tags = sentry_value_new_object();
	sentry_value_set_by_key(tags, "feature", sentry_value_new_string("rmbg_streaming"));
	sentry_value_set_by_key(tags, "ws_close_code", sentry_value_new_string(codeStr.c_str()));
	sentry_value_set_by_key(event, "tags", tags);

	sentry_value_t ctx = sentry_value_new_object();
	sentry_value_set_by_key(ctx, "close_code", sentry_value_new_int32(code));
	sentry_value_set_by_key(ctx, "reason", sentry_value_new_string(reason.c_str()));
	sentry_value_set_by_key(ctx, "reconnect_count", sentry_value_new_int32(reconnectCount));
	sentry_value_t contexts = sentry_value_new_object();
	sentry_value_set_by_key(contexts, "websocket", ctx);
	sentry_value_set_by_key(event, "contexts", contexts);

	sentry_capture_event(event);
}

void captureWsError(const std::string &reason)
{
	const std::string msg = "WebSocket error: " + reason;
	sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_ERROR, "ws", msg.c_str());

	sentry_value_t tags = sentry_value_new_object();
	sentry_value_set_by_key(tags, "feature", sentry_value_new_string("rmbg_streaming"));
	sentry_value_set_by_key(event, "tags", tags);

	sentry_capture_event(event);
}

void captureAuthDecryptFailed(const std::string &stage)
{
	const std::string msg = "SSO token decrypt failed at stage: " + stage;
	sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_ERROR, "auth", msg.c_str());

	sentry_value_t tags = sentry_value_new_object();
	sentry_value_set_by_key(tags, "feature", sentry_value_new_string("sso_auth"));
	sentry_value_set_by_key(tags, "decrypt_stage", sentry_value_new_string(stage.c_str()));
	sentry_value_set_by_key(event, "tags", tags);

	sentry_capture_event(event);
}

void captureAuthPollError(const std::string &serverError)
{
	const std::string msg = "SSO poll error: " + serverError;
	sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_ERROR, "auth", msg.c_str());

	sentry_value_t tags = sentry_value_new_object();
	sentry_value_set_by_key(tags, "feature", sentry_value_new_string("sso_auth"));
	sentry_value_set_by_key(event, "tags", tags);

	sentry_value_t ctx = sentry_value_new_object();
	sentry_value_set_by_key(ctx, "server_error", sentry_value_new_string(serverError.c_str()));
	sentry_value_t contexts = sentry_value_new_object();
	sentry_value_set_by_key(contexts, "auth", ctx);
	sentry_value_set_by_key(event, "contexts", contexts);

	sentry_capture_event(event);
}

void captureAuthRenewalFailed()
{
	sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_WARNING, "auth", "SSO token renewal failed");

	sentry_value_t tags = sentry_value_new_object();
	sentry_value_set_by_key(tags, "feature", sentry_value_new_string("sso_auth"));
	sentry_value_set_by_key(event, "tags", tags);

	sentry_capture_event(event);
}

void captureShaderLoadFailed(const std::string &path)
{
	const std::string msg = "Shader failed to load: " + path;
	sentry_value_t event = sentry_value_new_message_event(SENTRY_LEVEL_ERROR, "render", msg.c_str());

	sentry_value_t tags = sentry_value_new_object();
	sentry_value_set_by_key(tags, "feature", sentry_value_new_string("render"));
	sentry_value_set_by_key(event, "tags", tags);

	sentry_value_t ctx = sentry_value_new_object();
	sentry_value_set_by_key(ctx, "path", sentry_value_new_string(path.c_str()));
	sentry_value_t contexts = sentry_value_new_object();
	sentry_value_set_by_key(contexts, "shader", ctx);
	sentry_value_set_by_key(event, "contexts", contexts);

	sentry_capture_event(event);
}

void captureException(const std::string &location, const std::string &what)
{
	sentry_value_t event = sentry_value_new_event();
	sentry_value_set_by_key(event, "level", sentry_value_new_string("error"));

	sentry_value_t tags = sentry_value_new_object();
	sentry_value_set_by_key(tags, "location", sentry_value_new_string(location.c_str()));
	sentry_value_set_by_key(event, "tags", tags);

	sentry_value_t exc = sentry_value_new_object();
	sentry_value_set_by_key(exc, "type", sentry_value_new_string("std::exception"));
	sentry_value_set_by_key(exc, "value", sentry_value_new_string(what.c_str()));
	sentry_value_set_by_key(exc, "stacktrace", sentry_value_new_stacktrace(NULL, 0));

	sentry_value_t exc_list = sentry_value_new_list();
	sentry_value_append(exc_list, exc);
	sentry_value_set_by_key(event, "exception", exc_list);

	sentry_capture_event(event);
}

void captureFpsReport(double submittedFps, uint64_t framesSubmitted, uint64_t framesDropped)
{
	const std::string orgId = BriaAuthClient::instance().getOrgId();

	auto make_tags = [&]() {
		sentry_value_t t = sentry_value_new_object();
		sentry_value_set_by_key(t, "feature", sentry_value_new_string("obs_srmbg_plugin"));
		sentry_value_set_by_key(t, "org_id", sentry_value_new_string(orgId.c_str()));
		return t;
	};

	sentry_metrics_gauge("submitted_fps", submittedFps, "fps", make_tags());
	sentry_metrics_gauge("frames_submitted", (double)framesSubmitted, "frame", make_tags());
	sentry_metrics_gauge("frames_dropped", (double)framesDropped, "frame", make_tags());
}

} // namespace BriaSentry
