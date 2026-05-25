#ifndef BRIA_AUTH_CLIENT_H
#define BRIA_AUTH_CLIENT_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

/**
 * Manages Bria SSO authentication for the plugin.
 *
 *   1. generateSessionId()  →  obs-<random>
 *   2. Open https://platform.bria.ai/plugin-login?pluginAuthId=<id> in system browser
 *   3. Poll https://platform-api.bria.ai/plugins/auth/token_status every 2 s
 *   4. On success: AES-256-CBC decrypt (key = SHA256("+__BRIA.ai__+"), IV = first 16 bytes)
 *   5. Parse JSON → extract api_token, org_name, user_email
 *   6. Persist session_id + encrypted_token in OBS global config
 *   7. Renew on expiry via /renew_token
 */
class BriaAuthClient {
public:
	struct AuthData {
		std::string apiToken;
		std::string orgName;
		std::string orgId;
		std::string userEmail;
		std::string userName;
	};

	static BriaAuthClient &instance();

	~BriaAuthClient();

	// Start the SSO flow: opens the system browser and begins polling.
	void startLoginFlow();

	// Cancel an in-progress login flow.
	void cancelLoginFlow();

	// Sign out and clear all stored credentials.
	void logout();

	bool isAuthenticated() const;
	bool isCheckingAuth() const;

	std::string getApiToken() const;
	std::string getOrgName() const;
	std::string getUserEmail() const;

	// Register/unregister callbacks fired when auth state changes.
	// Callbacks may be called from the background poll thread.
	using Callback = std::function<void()>;
	using CallbackHandle = size_t;
	CallbackHandle addCallback(Callback cb);
	void removeCallback(CallbackHandle handle);

	// Persist/restore encrypted token + session id to/from OBS global config.
	void loadFromConfig();
	void saveToConfig() const;

private:
	BriaAuthClient();
	BriaAuthClient(const BriaAuthClient &) = delete;
	BriaAuthClient &operator=(const BriaAuthClient &) = delete;

	void runPollLoop(std::string sessionId);
	bool pollOnce(const std::string &sessionId, std::string &outEncToken, std::string &outError);
	bool renewTokenRequest(const std::string &sessionId, const std::string &encToken, std::string &outEncToken);
	bool decryptToken(const std::string &encToken, AuthData &out) const;

	void setAuthenticated(AuthData data, const std::string &encToken);
	void setCheckingAuth(bool checking);
	void clearAuth();
	void notifyCallbacks();

	static std::string generateSessionId();
	static void openSystemBrowser(const std::string &url);
	static std::string httpGet(const std::string &url);
	static std::string httpPost(const std::string &url, const std::string &jsonBody);
	static std::string extractJsonString(const std::string &json, const std::string &key);

	static constexpr const char *BASE_URL = "https://platform-api.bria.ai/plugins/auth";
	static constexpr const char *LOGIN_URL = "https://platform.bria.ai/plugin-login";
	static constexpr int POLL_INTERVAL_MS = 2000;
	static constexpr int MAX_CONSECUTIVE_ERRORS = 30;

	mutable std::mutex stateMutex_;
	AuthData authData_;
	std::string sessionId_;
	std::string encryptedToken_;

	std::atomic<bool> authenticated_{false};
	std::atomic<bool> checkingAuth_{false};
	std::atomic<bool> stopPoll_{false};

	std::thread pollThread_;

	std::mutex callbackMutex_;
	std::unordered_map<CallbackHandle, Callback> callbacks_;
	CallbackHandle nextHandle_{0};
};

#endif /* BRIA_AUTH_CLIENT_H */
