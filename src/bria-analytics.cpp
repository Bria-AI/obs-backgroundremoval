// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "bria-analytics.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <curl/curl.h>

#include <filesystem>
#include <random>
#include <sstream>
#include <thread>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr const char *POSTHOG_ENDPOINT = "https://eu.i.posthog.com/capture/";
#ifndef BRIA_POSTHOG_API_KEY
#error "BRIA_POSTHOG_API_KEY must be defined at compile time. Set the BRIA_POSTHOG_API_KEY env var before running cmake."
#endif
static constexpr const char *POSTHOG_API_KEY = BRIA_POSTHOG_API_KEY;
static constexpr const char *PLUGIN_ID = "bria-obs-backgroundremoval";
static constexpr const char *CONFIG_SECTION = "analytics";
static constexpr const char *CONFIG_KEY_DISTINCT_ID = "distinct_id";

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

BriaAnalytics &BriaAnalytics::instance()
{
	static BriaAnalytics inst;
	return inst;
}

BriaAnalytics::BriaAnalytics()
{
	loadOrCreateDistinctId();
}

// ---------------------------------------------------------------------------
// Distinct-ID persistence (stored in the plugin's config.ini)
// ---------------------------------------------------------------------------

void BriaAnalytics::loadOrCreateDistinctId()
{
	// Ensure the config folder exists
	char *folderPath = obs_module_config_path(NULL);
	if (folderPath) {
		os_mkdirs(folderPath);
		bfree(folderPath);
	}

	char *configFilePath = obs_module_config_path("config.ini");
	if (!configFilePath) {
		distinctId_ = generateUUID();
		return;
	}

	config_t *config = nullptr;
	int ret = config_open(&config, configFilePath, CONFIG_OPEN_ALWAYS);
	bfree(configFilePath);

	if (ret != CONFIG_SUCCESS || !config) {
		distinctId_ = generateUUID();
		return;
	}

	const char *stored = config_get_string(config, CONFIG_SECTION, CONFIG_KEY_DISTINCT_ID);
	if (stored && stored[0] != '\0') {
		distinctId_ = stored;
	} else {
		distinctId_ = generateUUID();
		config_set_string(config, CONFIG_SECTION, CONFIG_KEY_DISTINCT_ID, distinctId_.c_str());
		config_save(config);
	}

	config_close(config);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BriaAnalytics::identify(const std::string &email, const std::string &userName, const std::string &orgId,
			     const std::string &orgName)
{
	if (email.empty())
		return;

	std::string anonId;
	{
		std::lock_guard<std::mutex> lk(userMutex_);
		anonId = distinctId_;
		// Switch all future events to use email as distinct_id
		distinctId_ = email;
		userEmail_ = email;
		userName_ = userName;
		orgId_ = orgId;
		orgName_ = orgName;
	}

	// Build PostHog $identify payload to:
	//  1. Merge the anonymous UUID into the email-keyed user profile
	//  2. Set user-level properties visible in the PostHog People tab
	std::ostringstream json;
	json << "{";
	json << "\"api_key\":\"" << escapeJson(POSTHOG_API_KEY) << "\",";
	json << "\"event\":\"$identify\",";
	json << "\"distinct_id\":\"" << escapeJson(email) << "\",";
	json << "\"properties\":{";
	json << "\"$anon_distinct_id\":\"" << escapeJson(anonId) << "\",";
	json << "\"$set\":{";
	json << "\"email\":\"" << escapeJson(email) << "\"";
	if (!userName.empty())
		json << ",\"name\":\"" << escapeJson(userName) << "\"";
	if (!orgId.empty())
		json << ",\"org_id\":\"" << escapeJson(orgId) << "\"";
	if (!orgName.empty())
		json << ",\"org_name\":\"" << escapeJson(orgName) << "\"";
	json << "}}}";

	postAsync(json.str());
}

void BriaAnalytics::capture(const std::string &event, std::map<std::string, std::string> properties)
{
	// Merge super-properties
	properties["plugin"] = PLUGIN_ID;
	properties["plugin_version"] = PLUGIN_VERSION;

	std::string id, email, orgId;
	{
		std::lock_guard<std::mutex> lk(userMutex_);
		id = distinctId_;
		email = userEmail_;
		orgId = orgId_;
	}
	if (!email.empty())
		properties["email"] = email;
	if (!orgId.empty())
		properties["org_id"] = orgId;

	postAsync(buildPayload(event, id, properties));
}

// ---------------------------------------------------------------------------
// Async HTTP POST
// ---------------------------------------------------------------------------

void BriaAnalytics::postAsync(std::string payload)
{
	std::thread([payload = std::move(payload)]() {
		CURL *curl = curl_easy_init();
		if (!curl)
			return;

		struct curl_slist *headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		curl_easy_setopt(curl, CURLOPT_URL, POSTHOG_ENDPOINT);
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		// Discard response body — never written to any buffer or log
		curl_easy_setopt(
			curl, CURLOPT_WRITEFUNCTION,
			+[](char *, size_t size, size_t nmemb, void *) -> size_t { return size * nmemb; });

		curl_easy_perform(curl);

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
	}).detach();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string BriaAnalytics::generateUUID()
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<int> hex(0, 15);
	std::uniform_int_distribution<int> variant(8, 11);

	std::ostringstream ss;
	ss << std::hex;
	for (int i = 0; i < 8; ++i)
		ss << hex(gen);
	ss << '-';
	for (int i = 0; i < 4; ++i)
		ss << hex(gen);
	ss << "-4"; // version 4
	for (int i = 0; i < 3; ++i)
		ss << hex(gen);
	ss << '-';
	ss << variant(gen);
	for (int i = 0; i < 3; ++i)
		ss << hex(gen);
	ss << '-';
	for (int i = 0; i < 12; ++i)
		ss << hex(gen);
	return ss.str();
}

std::string BriaAnalytics::escapeJson(const std::string &s)
{
	std::ostringstream out;
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out << "\\\"";
			break;
		case '\\':
			out << "\\\\";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			if (c < 0x20) {
				out << "\\u00" << std::hex << (c >> 4) << (c & 0xf);
			} else {
				out << c;
			}
		}
	}
	return out.str();
}

std::string BriaAnalytics::buildPayload(const std::string &event, const std::string &distinctId,
					const std::map<std::string, std::string> &properties)
{
	std::ostringstream json;
	json << "{";
	json << "\"api_key\":\"" << escapeJson(POSTHOG_API_KEY) << "\",";
	json << "\"event\":\"" << escapeJson(event) << "\",";
	json << "\"distinct_id\":\"" << escapeJson(distinctId) << "\",";
	json << "\"properties\":{";
	bool first = true;
	for (const auto &kv : properties) {
		if (!first)
			json << ",";
		json << "\"" << escapeJson(kv.first) << "\":\"" << escapeJson(kv.second) << "\"";
		first = false;
	}
	json << "}}";
	return json.str();
}
