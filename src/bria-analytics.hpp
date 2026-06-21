// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <mutex>
#include <string>

// Lightweight singleton that fires PostHog capture events over HTTPS using
// libcurl.  Each call to capture() spawns a detached thread so it never
// blocks the OBS UI or video threads.
//
// Usage:
//   BriaAnalytics::instance().capture("obs_plugin_sign_in_clicked");
//   BriaAnalytics::instance().identify(email, userName, orgId, orgName);
class BriaAnalytics {
public:
	static BriaAnalytics &instance();

	// Fire-and-forget: sends the PostHog event on a detached thread.
	// Properties are merged with the default super-properties (plugin,
	// plugin_version, distinct_id, email, org_id).
	void capture(const std::string &event, std::map<std::string, std::string> properties = {});

	// Call after successful auth to link the anonymous distinct_id to the
	// user's email, set user profile properties in PostHog, and switch all
	// subsequent events to use email as distinct_id.
	void identify(const std::string &email, const std::string &userName, const std::string &orgId,
		      const std::string &orgName);

private:
	BriaAnalytics();

	mutable std::mutex userMutex_;
	std::string distinctId_; // starts as UUID, becomes email after identify()
	std::string userEmail_;
	std::string userName_;
	std::string orgId_;
	std::string orgName_;

	void loadOrCreateDistinctId();
	void postAsync(std::string payload);

	static std::string generateUUID();
	static std::string escapeJson(const std::string &s);
	static std::string buildPayload(const std::string &event, const std::string &distinctId,
					const std::map<std::string, std::string> &properties);
};
