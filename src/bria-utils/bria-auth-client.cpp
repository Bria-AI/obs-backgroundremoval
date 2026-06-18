// SPDX-FileCopyrightText: 2026 Bria AI <support@bria.ai>
//
// SPDX-License-Identifier: CC0-1.0

#include "bria-auth-client.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <fstream>
#include <sstream>

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

static size_t curlWriteFunc(void *ptr, size_t size, size_t nmemb, std::string *out)
{
	out->append(static_cast<char *>(ptr), size * nmemb);
	return size * nmemb;
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

BriaAuthClient &BriaAuthClient::instance()
{
	static BriaAuthClient client;
	return client;
}

BriaAuthClient::BriaAuthClient()
{
	loadFromConfig();
}

BriaAuthClient::~BriaAuthClient()
{
	stopPoll_.store(true);
	if (pollThread_.joinable()) {
		pollThread_.join();
	}
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void BriaAuthClient::startLoginFlow()
{
	cancelLoginFlow();

	const std::string sessionId = generateSessionId();

	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		sessionId_ = sessionId;
		encryptedToken_.clear();
	}

	checkingAuth_.store(true);
	stopPoll_.store(false);
	saveToConfig();
	notifyCallbacks();

	openSystemBrowser(std::string(LOGIN_URL) + "?pluginAuthId=" + sessionId);

	pollThread_ = std::thread(&BriaAuthClient::runPollLoop, this, sessionId);
}

void BriaAuthClient::cancelLoginFlow()
{
	stopPoll_.store(true);
	if (pollThread_.joinable()) {
		pollThread_.join();
	}
	checkingAuth_.store(false);
	notifyCallbacks();
}

void BriaAuthClient::logout()
{
	cancelLoginFlow();

	// Clear auth state immediately so the UI reflects it without delay.
	std::string sessionId, encToken;
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		sessionId = sessionId_;
		encToken = encryptedToken_;
	}

	clearAuth();
	saveToConfig();
	notifyCallbacks();
	obs_log(LOG_INFO, "Bria SSO: signed out");

	// Fire-and-forget: notify the server in the background so we don't block the UI.
	if (!sessionId.empty() && !encToken.empty()) {
		std::thread([sid = std::move(sessionId), enc = std::move(encToken)]() {
			const std::string body = "{\"plugin_auth_id\":\"" + sid + "\",\"token\":\"" + enc + "\"}";
			httpPost(std::string(BASE_URL) + "/logout", body);
		}).detach();
	}
}

bool BriaAuthClient::isAuthenticated() const
{
	return authenticated_.load();
}

bool BriaAuthClient::isCheckingAuth() const
{
	return checkingAuth_.load();
}

std::string BriaAuthClient::getApiToken() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return authData_.apiToken;
}

std::string BriaAuthClient::getOrgName() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return authData_.orgName;
}

std::string BriaAuthClient::getUserEmail() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return authData_.userEmail;
}

std::string BriaAuthClient::getOrgId() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return authData_.orgId;
}

std::string BriaAuthClient::getUserName() const
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	return authData_.userName;
}

BriaAuthClient::CallbackHandle BriaAuthClient::addCallback(Callback cb)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	const CallbackHandle handle = nextHandle_++;
	callbacks_[handle] = std::move(cb);
	return handle;
}

void BriaAuthClient::removeCallback(CallbackHandle handle)
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	callbacks_.erase(handle);
}

// ---------------------------------------------------------------------------
// Config persistence (OBS global config, section "BriaPlugin")
// ---------------------------------------------------------------------------

static std::string getAuthFilePath()
{
	char *rawPath = obs_module_config_path("bria_auth.txt");
	if (!rawPath) {
		return {};
	}
	std::string path(rawPath);
	bfree(rawPath);
	return path;
}

void BriaAuthClient::loadFromConfig()
{
	const std::string path = getAuthFilePath();
	if (path.empty()) {
		return;
	}

	std::ifstream file(path);
	if (!file.is_open()) {
		return;
	}

	std::string sessionId, encToken;
	std::getline(file, sessionId);
	std::getline(file, encToken);

	if (sessionId.empty() || encToken.empty()) {
		return;
	}

	AuthData data;
	if (decryptToken(encToken, data)) {
		setAuthenticated(data, encToken);
		{
			std::lock_guard<std::mutex> lock(stateMutex_);
			sessionId_ = sessionId;
		}
		obs_log(LOG_INFO, "Bria SSO: restored session for %s (%s)", data.userEmail.c_str(),
			data.orgName.c_str());
		return;
	}

	// Token may have expired — try renewal in the background
	{
		std::lock_guard<std::mutex> lock(stateMutex_);
		sessionId_ = sessionId;
		encryptedToken_ = encToken;
	}
	checkingAuth_.store(true);
	stopPoll_.store(false);

	pollThread_ = std::thread([this, sid = sessionId, enc2 = encToken]() {
		std::string newEncToken;
		if (renewTokenRequest(sid, enc2, newEncToken)) {
			AuthData data2;
			if (decryptToken(newEncToken, data2)) {
				setAuthenticated(data2, newEncToken);
				saveToConfig();
				obs_log(LOG_INFO, "Bria SSO: renewed token for %s", data2.userEmail.c_str());
				notifyCallbacks();
				return;
			}
		}
		checkingAuth_.store(false);
		obs_log(LOG_INFO, "Bria SSO: stored token expired, please sign in again");
		notifyCallbacks();
	});
}

void BriaAuthClient::saveToConfig() const
{
	const std::string path = getAuthFilePath();
	if (path.empty()) {
		return;
	}

	std::lock_guard<std::mutex> lock(stateMutex_);
	std::ofstream file(path, std::ios::trunc);
	if (file.is_open()) {
		file << sessionId_ << "\n" << encryptedToken_ << "\n";
	}
}

// ---------------------------------------------------------------------------
// Poll loop
// ---------------------------------------------------------------------------

void BriaAuthClient::runPollLoop(std::string sessionId)
{
	int consecutiveErrors = 0;

	// 1-second delay before first poll (give the browser time to load)
	std::this_thread::sleep_for(std::chrono::milliseconds(1000));

	while (!stopPoll_.load()) {
		std::string encToken, error;
		if (pollOnce(sessionId, encToken, error)) {
			// Got a token
			std::string newEnc;

			if (error == "Token expired") {
				std::lock_guard<std::mutex> lock(stateMutex_);
				const std::string stored = encryptedToken_;
				if (!stored.empty()) {
					if (renewTokenRequest(sessionId, stored, newEnc)) {
						encToken = newEnc;
					} else {
						obs_log(LOG_WARNING,
							"Bria SSO: token renewal failed, please sign in again");
						clearAuth();
						saveToConfig();
						notifyCallbacks();
						return;
					}
				}
			} else {
				newEnc = encToken;
			}

			AuthData data;
			if (!newEnc.empty() && decryptToken(newEnc, data)) {
				setAuthenticated(data, newEnc);
				saveToConfig();
				notifyCallbacks();
				obs_log(LOG_INFO, "Bria SSO: signed in as %s (%s)", data.userEmail.c_str(),
					data.orgName.c_str());
				return;
			}

			obs_log(LOG_ERROR, "Bria SSO: failed to decrypt token");
			clearAuth();
			notifyCallbacks();
			return;
		}

		// error is set — check type
		if (error == "Plugin Auth ID not found") {
			consecutiveErrors++;
			if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
				obs_log(LOG_WARNING, "Bria SSO: login session timed out");
				checkingAuth_.store(false);
				notifyCallbacks();
				return;
			}
		} else if (!error.empty() && error != "Plugin Auth ID not found") {
			obs_log(LOG_ERROR, "Bria SSO: poll error: %s", error.c_str());
			checkingAuth_.store(false);
			notifyCallbacks();
			return;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
	}
}

bool BriaAuthClient::pollOnce(const std::string &sessionId, std::string &outEncToken, std::string &outError)
{
	const std::string url = std::string(BASE_URL) + "/token_status?plugin_auth_id=" + sessionId;
	const std::string resp = httpGet(url);

	if (resp.empty()) {
		outError = "Network error";
		return false;
	}

	const std::string token = extractJsonString(resp, "token");
	if (!token.empty()) {
		outEncToken = token;
		return true;
	}

	outError = extractJsonString(resp, "error");
	// Return true on "Token expired" so the caller can attempt renewal
	if (outError == "Token expired") {
		return true;
	}
	return false;
}

bool BriaAuthClient::renewTokenRequest(const std::string &sessionId, const std::string &encToken,
				       std::string &outEncToken)
{
	const std::string body = "{\"plugin_auth_id\":\"" + sessionId + "\",\"token\":\"" + encToken + "\"}";
	const std::string resp = httpPost(std::string(BASE_URL) + "/renew_token", body);

	if (resp.empty()) {
		return false;
	}

	const std::string newToken = extractJsonString(resp, "token");
	if (newToken.empty()) {
		return false;
	}

	outEncToken = newToken;
	return true;
}

bool BriaAuthClient::decryptToken(const std::string &encToken, AuthData &out) const
{
	if (encToken.empty()) {
		obs_log(LOG_DEBUG, "Bria SSO decrypt: token is empty");
		return false;
	}

	// Normalise: strip whitespace, convert URL-safe base64 chars to standard
	std::string normalised;
	normalised.reserve(encToken.size());
	for (unsigned char c : encToken) {
		if (c == '-') {
			normalised += '+';
		} else if (c == '_') {
			normalised += '/';
		} else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			// skip whitespace
		} else {
			normalised += static_cast<char>(c);
		}
	}
	// Add base64 padding if missing
	while (normalised.size() % 4 != 0) {
		normalised += '=';
	}

	obs_log(LOG_DEBUG, "Bria SSO decrypt: token len=%zu normalised_len=%zu", encToken.size(), normalised.size());

	// Base64-decode
	size_t decodedLen = 0;
	const size_t maxDecoded = (normalised.size() * 3) / 4 + 4;
	std::vector<uint8_t> decoded(maxDecoded);

	const int b64Ret = mbedtls_base64_decode(decoded.data(), decoded.size(), &decodedLen,
						 reinterpret_cast<const uint8_t *>(normalised.data()),
						 normalised.size());
	if (b64Ret != 0) {
		obs_log(LOG_ERROR, "Bria SSO decrypt: base64 decode failed (ret=%d, token_len=%zu)", b64Ret,
			normalised.size());
		return false;
	}
	decoded.resize(decodedLen);

	obs_log(LOG_DEBUG, "Bria SSO decrypt: decoded %zu bytes", decodedLen);

	if (decoded.size() <= 16) {
		obs_log(LOG_ERROR, "Bria SSO decrypt: decoded blob too short (%zu bytes)", decoded.size());
		return false;
	}

	// IV = first 16 bytes
	uint8_t iv[16];
	std::memcpy(iv, decoded.data(), 16);

	const uint8_t *ciphertext = decoded.data() + 16;
	const size_t ciphertextLen = decoded.size() - 16;

	if (ciphertextLen == 0 || ciphertextLen % 16 != 0) {
		obs_log(LOG_ERROR, "Bria SSO decrypt: invalid ciphertext length %zu (not a multiple of 16)",
			ciphertextLen);
		return false;
	}

	uint8_t key[32];
#ifndef BRIA_SSO_SECRET
#error "BRIA_SSO_SECRET must be defined at compile time. Set the BRIA_SSO_SECRET env var before running cmake."
#endif
	static const char secret[] = BRIA_SSO_SECRET;
	mbedtls_sha256(reinterpret_cast<const uint8_t *>(secret), sizeof(secret) - 1, key, 0);

	// AES-256-CBC decrypt
	mbedtls_aes_context aes;
	mbedtls_aes_init(&aes);
	if (mbedtls_aes_setkey_dec(&aes, key, 256) != 0) {
		mbedtls_aes_free(&aes);
		obs_log(LOG_ERROR, "Bria SSO decrypt: AES setkey failed");
		return false;
	}

	std::vector<uint8_t> plaintext(ciphertextLen);
	const int aesRet =
		mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ciphertextLen, iv, ciphertext, plaintext.data());
	mbedtls_aes_free(&aes);

	if (aesRet != 0) {
		obs_log(LOG_ERROR, "Bria SSO decrypt: AES-CBC decrypt failed (ret=%d)", aesRet);
		return false;
	}

	// PKCS7 unpad
	if (plaintext.empty()) {
		obs_log(LOG_ERROR, "Bria SSO decrypt: plaintext is empty after AES");
		return false;
	}
	const uint8_t padLen = plaintext.back();
	if (padLen == 0 || padLen > 16 || padLen > plaintext.size()) {
		obs_log(LOG_ERROR, "Bria SSO decrypt: bad PKCS7 pad byte %u (plaintext_len=%zu)", (unsigned)padLen,
			plaintext.size());
		return false;
	}
	plaintext.resize(plaintext.size() - padLen);

	const std::string json(plaintext.begin(), plaintext.end());

	obs_log(LOG_DEBUG, "Bria SSO decrypt: plaintext length %zu", json.size());

	out.apiToken = extractJsonString(json, "api_token");
	out.orgName = extractJsonString(json, "org_name");
	out.orgId = extractJsonString(json, "org_id");
	out.userEmail = extractJsonString(json, "user_email");
	out.userName = extractJsonString(json, "user_name");

	if (out.apiToken.empty()) {
		obs_log(LOG_ERROR, "Bria SSO decrypt: api_token missing from decrypted JSON");
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

void BriaAuthClient::setAuthenticated(AuthData data, const std::string &encToken)
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	authData_ = std::move(data);
	encryptedToken_ = encToken;
	authenticated_.store(true);
	checkingAuth_.store(false);
}

void BriaAuthClient::setCheckingAuth(bool checking)
{
	checkingAuth_.store(checking);
}

void BriaAuthClient::clearAuth()
{
	std::lock_guard<std::mutex> lock(stateMutex_);
	authData_ = {};
	encryptedToken_.clear();
	authenticated_.store(false);
	checkingAuth_.store(false);
}

void BriaAuthClient::notifyCallbacks()
{
	std::lock_guard<std::mutex> lock(callbackMutex_);
	for (auto &[handle, cb] : callbacks_) {
		if (cb) {
			cb();
		}
	}
}

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

std::string BriaAuthClient::generateSessionId()
{
	static std::atomic<uint64_t> counter{0};
	const uint64_t id = counter.fetch_add(1);
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	char buf[32];
	snprintf(buf, sizeof(buf), "obs-%08llx%08llx", static_cast<unsigned long long>(now & 0xFFFFFFFF),
		 static_cast<unsigned long long>(id));
	return buf;
}

void BriaAuthClient::openSystemBrowser(const std::string &url)
{
#ifdef _WIN32
	const int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
	std::wstring wurl(wlen, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), wlen);
	ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
	const int ret = system(("open '" + url + "'").c_str());
	if (ret != 0)
		obs_log(LOG_WARNING, "Bria SSO: failed to open browser (ret=%d)", ret);
#else
	const int ret = system(("xdg-open '" + url + "' &").c_str());
	if (ret != 0)
		obs_log(LOG_WARNING, "Bria SSO: failed to open browser (ret=%d)", ret);
#endif
}

static const std::string &briaUserAgent()
{
	static const std::string ua = "BriaOBS/" PLUGIN_VERSION_STR;
	return ua;
}

std::string BriaAuthClient::httpGet(const std::string &url)
{
	CURL *curl = curl_easy_init();
	if (!curl) {
		return {};
	}

	std::string response;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteFunc);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, briaUserAgent().c_str());
	curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return response;
}

std::string BriaAuthClient::httpPost(const std::string &url, const std::string &jsonBody)
{
	CURL *curl = curl_easy_init();
	if (!curl) {
		return {};
	}

	std::string response;
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteFunc);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, briaUserAgent().c_str());

	curl_easy_perform(curl);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return response;
}

std::string BriaAuthClient::extractJsonString(const std::string &json, const std::string &key)
{
	// Accept both `"key":"value"` and `"key" : "value"` (spaces around colon)
	const std::string keyPart = "\"" + key + "\"";
	size_t pos = json.find(keyPart);
	if (pos == std::string::npos) {
		return {};
	}
	pos += keyPart.size();
	// Skip optional whitespace and colon
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
		++pos;
	}
	if (pos >= json.size() || json[pos] != ':') {
		return {};
	}
	++pos; // skip ':'
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
		++pos;
	}
	if (pos >= json.size() || json[pos] != '"') {
		return {};
	}
	++pos; // skip opening '"'
	const size_t start = pos;
	std::string result;
	for (size_t i = start; i < json.size(); ++i) {
		if (json[i] == '\\' && i + 1 < json.size()) {
			++i;
			if (json[i] == '"') {
				result += '"';
			} else if (json[i] == '\\') {
				result += '\\';
			} else {
				result += json[i];
			}
		} else if (json[i] == '"') {
			break;
		} else {
			result += json[i];
		}
	}
	return result;
}
