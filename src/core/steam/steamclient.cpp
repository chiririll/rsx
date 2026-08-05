#include <pch.h>

#include <atomic>
#include <chrono>
#include <condition_variable>

#include <core/steam/tek_includes.h>
#include <core/steam/steamclient.h>
#include <core/steam/steamcache.h>
#include <core/steam/binary_vdf.h>
#include <core/steam/steam_depot_util.h>
#include <core/steam/steam_token_blob.h>

#include <thirdparty/valvefilevdf/vdf_parser.hpp>

CSteamClient g_steamClient;

namespace
{
	constexpr long kDefaultTimeoutMs = 30000;
	constexpr const char* kTokenFileName = "rsx_steam_token.bin";
	constexpr const char* kDeviceName = "RSX";

	struct CallbackWaiter
	{
		std::mutex mutex;
		std::condition_variable cv;
		bool done = false;
		tek_sc_err result{};
		std::function<void(void*)> onData;

		void Reset()
		{
			std::lock_guard lock(mutex);
			done = false;
			result = {};
		}

		bool Wait(long timeoutMs, std::string& outError)
		{
			std::unique_lock lock(mutex);
			if (!cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return done; }))
			{
				outError = "Steam request timed out";
				return false;
			}
			return true;
		}

		static void OnCallback(tek_sc_cm_client* /*client*/, void* data, void* userData)
		{
			auto* self = static_cast<CallbackWaiter*>(userData);
			{
				std::lock_guard lock(self->mutex);
				if (self->onData)
					self->onData(data);
				self->done = true;
			}
			self->cv.notify_all();
		}
	};

	// tek-steamclient is a MinGW build that allocates via the release UCRT heap.
	// In Debug (/MDd) MSVC's free() goes through the debug CRT and will crash on
	// those pointers — always release tek-owned blocks through ucrtbase!free.
	void TekHeapFree(void* ptr)
	{
		if (!ptr)
			return;

		using FreeFn = void(__cdecl*)(void*);
		static FreeFn s_ucrtFree = []() -> FreeFn
			{
				HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
				if (!ucrt)
					ucrt = LoadLibraryW(L"ucrtbase.dll");
				if (!ucrt)
					return nullptr;
				return reinterpret_cast<FreeFn>(GetProcAddress(ucrt, "free"));
			}();

		if (s_ucrtFree)
			s_ucrtFree(ptr);
		else
			free(ptr);
	}

	struct TekHeapDeleter
	{
		void operator()(void* ptr) const { TekHeapFree(ptr); }
	};

	std::string FormatTekError(const tek_sc_err& err)
	{
		if (tek_sc_err_success(&err))
			return {};

		const tek_sc_err_msgs msgs = tek_sc_err_get_msgs(&err);
		std::string message = msgs.primary ? msgs.primary : "Unknown Steam error";
		if (msgs.auxiliary)
		{
			message += " (";
			message += msgs.auxiliary;
			message += ")";
		}
		if (err.uri)
		{
			message += " [";
			message += err.uri;
			message += "]";
			TekHeapFree(const_cast<char*>(err.uri));
		}
		return message;
	}
}

struct CSteamClient::Impl
{
	tek_sc_lib_ctx* lib = nullptr;
	tek_sc_cm_client* cm = nullptr;
	tek_sc_depot_manifest manifest{};
	bool hasManifest = false;
	tek_sc_aes256_key depotKey{};
	bool hasDepotKey = false;
	bool licensesFetched = false;
	tek_sc_sp_dec_ctx* decCtx = nullptr;

	std::vector<tek_sc_cm_sp_srv_entry> servers;
	// Host strings live in the allocation returned by get_sp_servers; keep the original block.
	void* serversAllocation = nullptr;

	std::unordered_map<std::string, const tek_sc_dm_file*> fileIndex; // normalized path -> file
	std::unordered_map<std::string, const tek_sc_dm_file*> basenameIndex;

	// In-flight chunk downloads keyed by sha1 hex.
	mutable std::mutex chunkMutex;
	mutable std::unordered_map<std::string, std::shared_ptr<std::condition_variable>> chunkWaiters;
	mutable std::unordered_set<std::string> chunkInFlight;

	CallbackWaiter waiter;
};

CSteamClient::CSteamClient()
	: m_impl(std::make_unique<Impl>())
{
}

CSteamClient::~CSteamClient()
{
	Shutdown();
}

bool CSteamClient::Init(std::string& outError)
{
	if (m_impl->lib)
		return true;

	m_impl->lib = tek_sc_lib_init(false, false);
	if (!m_impl->lib)
	{
		outError = "Failed to initialize tek-steamclient";
		return false;
	}

	m_impl->cm = tek_sc_cm_client_create(m_impl->lib, &m_impl->waiter);
	if (!m_impl->cm)
	{
		outError = "Failed to create Steam CM client";
		Shutdown();
		return false;
	}

	return true;
}

void CSteamClient::Shutdown()
{
	if (m_impl->decCtx)
	{
		tek_sc_sp_dec_ctx_destroy(m_impl->decCtx);
		m_impl->decCtx = nullptr;
	}

	if (m_impl->hasManifest)
	{
		tek_sc_dm_free(&m_impl->manifest);
		m_impl->hasManifest = false;
	}

	m_impl->fileIndex.clear();
	m_impl->basenameIndex.clear();
	m_impl->hasDepotKey = false;
	m_impl->licensesFetched = false;

	if (m_impl->serversAllocation)
	{
		TekHeapFree(m_impl->serversAllocation);
		m_impl->serversAllocation = nullptr;
	}
	m_impl->servers.clear();

	if (m_impl->cm)
	{
		if (m_connected)
			tek_sc_cm_disconnect(m_impl->cm);
		tek_sc_cm_client_destroy(m_impl->cm);
		m_impl->cm = nullptr;
	}

	if (m_impl->lib)
	{
		tek_sc_lib_cleanup(m_impl->lib);
		m_impl->lib = nullptr;
	}

	m_connected = false;
	m_signedIn = false;
	m_anonymous = false;
}

bool CSteamClient::EnsureConnected(std::string& outError)
{
	if (!Init(outError))
		return false;

	if (m_connected)
		return true;

	m_impl->waiter.Reset();
	m_impl->waiter.onData = [this](void* data)
		{
			const auto* err = static_cast<const tek_sc_err*>(data);
			m_impl->waiter.result = *err;
			if (tek_sc_err_success(err))
				m_connected = true;
		};

	tek_sc_cm_connect(m_impl->cm, &CallbackWaiter::OnCallback, kDefaultTimeoutMs,
		[](tek_sc_cm_client*, void* /*data*/, void* userData)
		{
			// userData is &Impl::waiter; recover Impl via offsetof-equivalent layout.
			auto* waiter = static_cast<CallbackWaiter*>(userData);
			// Mark disconnected - the owning CSteamClient sets m_connected=false on Logout/Shutdown.
			// Soft disconnect notification only.
			UNUSED(waiter);
		});

	if (!m_impl->waiter.Wait(kDefaultTimeoutMs, outError))
		return false;

	if (!tek_sc_err_success(&m_impl->waiter.result))
	{
		outError = FormatTekError(m_impl->waiter.result);
		m_connected = false;
		return false;
	}

	return true;
}

bool CSteamClient::LoginAnonymous(std::string& outError)
{
	if (!EnsureConnected(outError))
		return false;

	m_impl->waiter.Reset();
	m_impl->waiter.onData = [this](void* data)
		{
			m_impl->waiter.result = *static_cast<const tek_sc_err*>(data);
			if (tek_sc_err_success(&m_impl->waiter.result))
				m_signedIn = true;
		};

	tek_sc_cm_sign_in_anon(m_impl->cm, &CallbackWaiter::OnCallback, kDefaultTimeoutMs);
	if (!m_impl->waiter.Wait(kDefaultTimeoutMs, outError))
		return false;

	if (!tek_sc_err_success(&m_impl->waiter.result))
	{
		outError = FormatTekError(m_impl->waiter.result);
		return false;
	}

	m_username.clear();
	m_anonymous = true;
	m_impl->licensesFetched = false;
	EnsureLicenses(outError); // best-effort; anonymous rarely needs package tokens
	outError.clear();
	return true;
}

bool CSteamClient::LoginWithToken(const std::string& token, std::string& outError)
{
	if (!EnsureConnected(outError))
		return false;

	m_impl->waiter.Reset();
	m_impl->waiter.onData = [this](void* data)
		{
			m_impl->waiter.result = *static_cast<const tek_sc_err*>(data);
			if (tek_sc_err_success(&m_impl->waiter.result))
				m_signedIn = true;
		};

	tek_sc_cm_sign_in(m_impl->cm, token.c_str(), &CallbackWaiter::OnCallback, kDefaultTimeoutMs);
	if (!m_impl->waiter.Wait(kDefaultTimeoutMs, outError))
		return false;

	if (!tek_sc_err_success(&m_impl->waiter.result))
	{
		outError = FormatTekError(m_impl->waiter.result);
		return false;
	}

	m_anonymous = false;
	m_impl->licensesFetched = false;
	if (!EnsureLicenses(outError))
		return false;

	return true;
}

bool CSteamClient::Login(const std::string& username, const std::string& password, bool rememberLogin,
	const GuardCodeCallback& guardCb, std::string& outError)
{
	if (rememberLogin)
	{
		std::string savedUser;
		std::string savedToken;
		if (LoadToken(savedUser, savedToken) && (username.empty() || savedUser == username))
		{
			if (LoginWithToken(savedToken, outError))
			{
				m_username = savedUser;
				return true;
			}
			ClearToken();
			outError.clear();
		}
	}

	if (!EnsureConnected(outError))
		return false;

	std::string authToken;
	bool authCompleted = false;
	bool needsCode = false;
	tek_sc_cm_auth_confirmation_type confType = TEK_SC_CM_AUTH_CONFIRMATION_TYPE_none;

	m_impl->waiter.Reset();
	m_impl->waiter.onData = [&](void* data)
		{
			const auto* polling = static_cast<const tek_sc_cm_data_auth_polling*>(data);
			if (polling->status == TEK_SC_CM_AUTH_STATUS_completed)
			{
				m_impl->waiter.result = polling->result;
				if (tek_sc_err_success(&polling->result) && polling->token)
					authToken = polling->token;
				authCompleted = true;
				m_impl->waiter.done = true;
			}
			else if (polling->status == TEK_SC_CM_AUTH_STATUS_awaiting_confirmation)
			{
				needsCode = true;
				confType = polling->confirmation_types;
				m_impl->waiter.done = true;
			}
			// new_url (QR) is ignored in credentials flow
		};

	tek_sc_cm_auth_credentials(m_impl->cm, kDeviceName, username.c_str(), password.c_str(),
		&CallbackWaiter::OnCallback, kDefaultTimeoutMs);

	// Auth may require multiple waiter cycles for guard codes.
	for (;;)
	{
		if (!m_impl->waiter.Wait(120000, outError)) // Steam Guard can take a while
			return false;

		if (authCompleted)
			break;

		if (needsCode)
		{
			if (!guardCb)
			{
				outError = "Steam Guard confirmation required but no code callback was provided";
				return false;
			}

			std::string prompt = "Enter Steam Guard code";
			if (confType & TEK_SC_CM_AUTH_CONFIRMATION_TYPE_email)
				prompt = "Enter Steam Guard email code";
			else if (confType & TEK_SC_CM_AUTH_CONFIRMATION_TYPE_guard_code)
				prompt = "Enter Steam Guard mobile code";
			else if (confType & TEK_SC_CM_AUTH_CONFIRMATION_TYPE_device)
				prompt = "Confirm in Steam mobile app, or enter code if prompted";

			const std::string code = guardCb(prompt);
			if (code.empty())
			{
				outError = "Steam Guard code entry cancelled";
				return false;
			}

			tek_sc_cm_auth_confirmation_type submitType = TEK_SC_CM_AUTH_CONFIRMATION_TYPE_guard_code;
			if (confType & TEK_SC_CM_AUTH_CONFIRMATION_TYPE_email)
				submitType = TEK_SC_CM_AUTH_CONFIRMATION_TYPE_email;
			else if (confType & TEK_SC_CM_AUTH_CONFIRMATION_TYPE_guard_code)
				submitType = TEK_SC_CM_AUTH_CONFIRMATION_TYPE_guard_code;

			m_impl->waiter.Reset();
			needsCode = false;
			const tek_sc_err submitErr = tek_sc_cm_auth_submit_code(m_impl->cm, submitType, code.c_str());
			if (!tek_sc_err_success(&submitErr))
			{
				outError = FormatTekError(submitErr);
				return false;
			}
			continue;
		}

		outError = "Unexpected Steam authentication state";
		return false;
	}

	if (!tek_sc_err_success(&m_impl->waiter.result) || authToken.empty())
	{
		outError = FormatTekError(m_impl->waiter.result);
		if (outError.empty())
			outError = "Steam authentication failed";
		return false;
	}

	if (!LoginWithToken(authToken, outError))
		return false;

	m_username = username;
	if (rememberLogin)
		SaveToken(username, authToken);

	return true;
}

bool CSteamClient::TryRestoreSession(std::string& outError)
{
	if (m_signedIn)
		return true;

	std::string savedUser;
	std::string savedToken;
	if (!LoadToken(savedUser, savedToken))
	{
		outError = "No saved Steam login token";
		return false;
	}

	if (LoginWithToken(savedToken, outError))
	{
		m_username = savedUser;
		Log("STEAM: Restored session for %s\n", savedUser.c_str());
		return true;
	}

	// Sign-in failed — try renewing the refresh token first.
	const std::string signInError = outError;
	if (!EnsureConnected(outError))
		return false;

	std::string renewedToken;
	m_impl->waiter.Reset();
	m_impl->waiter.onData = [&](void* data)
		{
			const auto* resp = static_cast<const tek_sc_cm_data_renew_token*>(data);
			m_impl->waiter.result = resp->result;
			if (tek_sc_err_success(&resp->result) && resp->new_token)
				renewedToken = resp->new_token;
		};

	tek_sc_cm_auth_renew_token(m_impl->cm, savedToken.c_str(), &CallbackWaiter::OnCallback, kDefaultTimeoutMs);
	if (m_impl->waiter.Wait(kDefaultTimeoutMs, outError) && !renewedToken.empty())
	{
		if (LoginWithToken(renewedToken, outError))
		{
			m_username = savedUser;
			SaveToken(savedUser, renewedToken);
			Log("STEAM: Renewed and restored session for %s\n", savedUser.c_str());
			return true;
		}
	}

	// Keep the old token file if renew quietly failed (Steam may leave it valid);
	// only clear when sign-in explicitly rejected it.
	outError = signInError.empty() ? "Saved Steam login expired; please sign in again" : signInError;
	ClearToken();
	return false;
}

bool CSteamClient::LoginWithQr(bool rememberLogin, const QrUrlCallback& onUrl,
	std::atomic<bool>* cancelFlag, std::string& outError)
{
	if (rememberLogin && TryRestoreSession(outError))
		return true;
	outError.clear();

	if (!EnsureConnected(outError))
		return false;

	std::string authToken;
	bool authCompleted = false;
	bool sawUrl = false;

	m_impl->waiter.Reset();
	m_impl->waiter.onData = [&](void* data)
		{
			const auto* polling = static_cast<const tek_sc_cm_data_auth_polling*>(data);
			if (polling->status == TEK_SC_CM_AUTH_STATUS_new_url)
			{
				if (polling->url && onUrl)
					onUrl(polling->url);
				sawUrl = true;
			}
			else if (polling->status == TEK_SC_CM_AUTH_STATUS_awaiting_confirmation)
			{
				// QR login usually waits for a confirmation tap in the Steam app.
				// Keep polling; no code entry required for the device confirmation path.
				if (onUrl && (polling->confirmation_types & TEK_SC_CM_AUTH_CONFIRMATION_TYPE_device))
					onUrl({}); // empty URL = "confirm in app" signal to UI
			}
			else if (polling->status == TEK_SC_CM_AUTH_STATUS_completed)
			{
				m_impl->waiter.result = polling->result;
				if (tek_sc_err_success(&polling->result) && polling->token)
					authToken = polling->token;
				authCompleted = true;
			}
		};

	tek_sc_cm_auth_qr(m_impl->cm, kDeviceName, &CallbackWaiter::OnCallback, kDefaultTimeoutMs);

	for (;;)
	{
		if (cancelFlag && cancelFlag->load())
		{
			outError = "QR login cancelled";
			return false;
		}

		if (!m_impl->waiter.Wait(180000, outError))
			return false;

		if (authCompleted)
			break;

		// Intermediate event (new QR URL or awaiting mobile confirmation) — keep waiting.
		m_impl->waiter.Reset();
	}

	if (!tek_sc_err_success(&m_impl->waiter.result) || authToken.empty())
	{
		outError = FormatTekError(m_impl->waiter.result);
		if (outError.empty())
			outError = sawUrl ? "QR login failed or timed out" : "Steam did not issue a QR login URL";
		return false;
	}

	if (!LoginWithToken(authToken, outError))
		return false;

	const tek_sc_cm_auth_token_info info = tek_sc_cm_parse_auth_token(authToken.c_str());
	m_username = info.steam_id != 0 ? std::to_string(info.steam_id) : "qr_login";
	if (rememberLogin)
		SaveToken(m_username, authToken);

	return true;
}

void CSteamClient::Logout()
{
	m_signedIn = false;
	m_anonymous = false;
	m_impl->licensesFetched = false;
	ClearDepotContext();
	if (m_connected && m_impl->cm)
		tek_sc_cm_disconnect(m_impl->cm);
	m_connected = false;
}

bool CSteamClient::HasRememberedToken() const
{
	std::string user, token;
	return LoadToken(user, token);
}

std::string CSteamClient::GetRememberedUsername() const
{
	std::string user, token;
	LoadToken(user, token);
	return user;
}

std::filesystem::path CSteamClient::TokenFilePath()
{
	// Always next to the executable — CWD can change (drag/drop, dialogs).
	wchar_t processPath[MAX_PATH]{};
	const DWORD n = GetModuleFileNameW(nullptr, processPath, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::filesystem::current_path() / kTokenFileName;
	return std::filesystem::path(processPath).parent_path() / kTokenFileName;
}

bool CSteamClient::SaveToken(const std::string& username, const std::string& token) const
{
	const std::vector<char> blob = SerializeAuthToken(username, token);
	StreamIO file;
	if (!file.open(TokenFilePath().string(), eStreamIOMode::Write))
		return false;
	file.write(blob.data(), blob.size());
	return true;
}

bool CSteamClient::LoadToken(std::string& username, std::string& token) const
{
	StreamIO file;
	if (!file.open(TokenFilePath().string(), eStreamIOMode::Read))
		return false;

	const size_t size = file.size();
	if (size == 0)
		return false;

	std::vector<char> blob(size);
	file.read(blob.data(), size);
	return DeserializeAuthToken(blob.data(), blob.size(), username, token);
}

void CSteamClient::ClearToken() const
{
	std::error_code ec;
	std::filesystem::remove(TokenFilePath(), ec);
}

std::string CSteamClient::Sha1ToHex(const unsigned char sha[20])
{
	static constexpr char kHex[] = "0123456789abcdef";
	std::string out(40, '0');
	for (int i = 0; i < 20; ++i)
	{
		out[i * 2] = kHex[(sha[i] >> 4) & 0xF];
		out[i * 2 + 1] = kHex[sha[i] & 0xF];
	}
	return out;
}

std::string CSteamClient::WideToUtf8(const wchar_t* wstr)
{
	if (!wstr || !*wstr)
		return {};
	const int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1)
		return {};
	std::string out(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
	return out;
}

std::wstring CSteamClient::Utf8ToWide(const std::string& str)
{
	if (str.empty())
		return {};
	const int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
	if (len <= 1)
		return {};
	std::wstring out(static_cast<size_t>(len - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, out.data(), len);
	return out;
}

std::string CSteamClient::NormalizeDepotPath(const std::string& path)
{
	return ::NormalizeDepotPath(path);
}

bool CSteamClient::EnsureLicenses(std::string& outError)
{
	if (m_impl->licensesFetched)
		return true;

	if (!m_signedIn)
	{
		outError = "Not signed in";
		return false;
	}

	// Steam authorizes depot keys against the account license list. Fetch it
	// after sign-in so GetDepotDecryptionKey does not fail with FileNotFound.
	m_impl->waiter.Reset();
	m_impl->waiter.onData = [&](void* data)
		{
			const auto* resp = static_cast<const tek_sc_cm_data_lics*>(data);
			m_impl->waiter.result = resp->result;
		};

	tek_sc_cm_get_licenses(m_impl->cm, &CallbackWaiter::OnCallback, kDefaultTimeoutMs);
	if (!m_impl->waiter.Wait(kDefaultTimeoutMs, outError))
		return false;

	if (!tek_sc_err_success(&m_impl->waiter.result))
	{
		outError = FormatTekError(m_impl->waiter.result);
		if (outError.empty())
			outError = "Failed to get Steam license list";
		return false;
	}

	m_impl->licensesFetched = true;
	return true;
}

bool CSteamClient::EnsureDepotKey(std::string& outError)
{
	if (m_impl->hasDepotKey)
		return true;

	if (m_anonymous)
	{
		outError = "Depot decryption keys require a full Steam login that owns the game (anonymous cannot decrypt)";
		return false;
	}

	if (!EnsureLicenses(outError))
		return false;

	if (g_steamCacheManager.TryGetDepotKey(m_ctx.depotId, m_impl->depotKey))
	{
		m_impl->hasDepotKey = true;
		tek_sc_lib_add_depot_key(m_impl->lib, m_ctx.depotId, m_impl->depotKey);
		return true;
	}

	const uint32_t keyAppId = m_ctx.keyAppId != 0 ? m_ctx.keyAppId : m_ctx.appId;

	tek_sc_cm_data_depot_key data{};
	data.app_id = keyAppId;
	data.depot_id = m_ctx.depotId;

	m_impl->waiter.Reset();
	m_impl->waiter.onData = [&](void* cbData)
		{
			const auto* resp = static_cast<const tek_sc_cm_data_depot_key*>(cbData);
			m_impl->waiter.result = resp->result;
			if (tek_sc_err_success(&resp->result))
				memcpy(m_impl->depotKey, resp->key, sizeof(m_impl->depotKey));
		};

	tek_sc_cm_get_depot_key(m_impl->cm, &data, &CallbackWaiter::OnCallback, kDefaultTimeoutMs);
	if (!m_impl->waiter.Wait(kDefaultTimeoutMs, outError))
		return false;

	if (!tek_sc_err_success(&m_impl->waiter.result))
	{
		outError = FormatTekError(m_impl->waiter.result);
		if (outError.empty())
			outError = "Failed to get depot decryption key";
		outError += " — account must own this app (QR/password login, not anonymous). "
			"Also verify app/depot IDs match SteamDB.";
		return false;
	}

	m_impl->hasDepotKey = true;
	tek_sc_lib_add_depot_key(m_impl->lib, m_ctx.depotId, m_impl->depotKey);
	g_steamCacheManager.StoreDepotKey(m_ctx.depotId, m_impl->depotKey);
	return true;
}

bool CSteamClient::EnsureServers(std::string& outError)
{
	if (!m_impl->servers.empty())
		return true;

	m_impl->waiter.Reset();
	m_impl->waiter.onData = [this](void* data)
		{
			const auto* resp = static_cast<const tek_sc_cm_data_sp_servers*>(data);
			m_impl->waiter.result = resp->result;
			if (tek_sc_err_success(&resp->result) && resp->entries && resp->num_entries > 0)
			{
				m_impl->serversAllocation = resp->entries;
				m_impl->servers.assign(resp->entries, resp->entries + resp->num_entries);
			}
		};

	tek_sc_cm_get_sp_servers(m_impl->cm, &CallbackWaiter::OnCallback, kDefaultTimeoutMs);
	if (!m_impl->waiter.Wait(kDefaultTimeoutMs, outError))
		return false;

	if (!tek_sc_err_success(&m_impl->waiter.result) || m_impl->servers.empty())
	{
		outError = FormatTekError(m_impl->waiter.result);
		if (outError.empty())
			outError = "Failed to get SteamPipe server list";
		return false;
	}

	return true;
}

bool CSteamClient::FetchManifest(uint64_t manifestId, std::string& outError)
{
	if (!EnsureDepotKey(outError) || !EnsureServers(outError))
		return false;

	// Cached serialized manifest?
	std::vector<char> cached;
	if (g_steamCacheManager.TryGetManifest(m_ctx.depotId, manifestId, cached))
	{
		if (m_impl->hasManifest)
		{
			tek_sc_dm_free(&m_impl->manifest);
			m_impl->hasManifest = false;
		}

		const tek_sc_err err = tek_sc_dm_deserialize(cached.data(), static_cast<int>(cached.size()), &m_impl->manifest);
		if (tek_sc_err_success(&err))
		{
			m_impl->hasManifest = true;
			m_impl->manifest.item_id.app_id = m_ctx.appId;
			m_impl->manifest.item_id.depot_id = m_ctx.depotId;
			m_impl->manifest.id = manifestId;
			return true;
		}
	}

	tek_sc_cm_data_mrc mrc{};
	mrc.app_id = m_ctx.appId;
	mrc.depot_id = m_ctx.depotId;
	mrc.manifest_id = manifestId;

	m_impl->waiter.Reset();
	m_impl->waiter.onData = [&](void* data)
		{
			const auto* resp = static_cast<const tek_sc_cm_data_mrc*>(data);
			m_impl->waiter.result = resp->result;
			mrc.request_code = resp->request_code;
		};

	if (!m_ctx.branch.empty() && m_ctx.branch != "public")
		tek_sc_cm_get_mrc_branch(m_impl->cm, &mrc, &CallbackWaiter::OnCallback, kDefaultTimeoutMs, m_ctx.branch.c_str());
	else
		tek_sc_cm_get_mrc(m_impl->cm, &mrc, &CallbackWaiter::OnCallback, kDefaultTimeoutMs);

	if (!m_impl->waiter.Wait(kDefaultTimeoutMs, outError))
		return false;

	if (!tek_sc_err_success(&m_impl->waiter.result) || mrc.request_code == 0)
	{
		outError = FormatTekError(m_impl->waiter.result);
		if (outError.empty())
			outError = "Steam refused this manifest version; it may be blocked or require an account that owns the app";
		return false;
	}

	tek_sc_sp_data_dm dm{};
	dm.common.srvs = m_impl->servers.data();
	dm.common.num_srvs = static_cast<int>(m_impl->servers.size());
	dm.common.cm_client = m_impl->cm;
	dm.common.depot_id = m_ctx.depotId;
	dm.manifest_id = manifestId;
	dm.request_code = mrc.request_code;

	std::atomic_bool cancel{ false };
	const tek_sc_err dlErr = tek_sc_sp_download_dm(&dm, kDefaultTimeoutMs, &cancel);
	if (!tek_sc_err_success(&dlErr) || !dm.common.data)
	{
		outError = FormatTekError(dlErr);
		if (outError.empty())
			outError = "Failed to download depot manifest";
		return false;
	}

	if (m_impl->hasManifest)
	{
		tek_sc_dm_free(&m_impl->manifest);
		m_impl->hasManifest = false;
	}

	const tek_sc_err parseErr = tek_sc_dm_parse(dm.common.data, dm.common.data_size, m_impl->depotKey, &m_impl->manifest);
	TekHeapFree(dm.common.data);

	if (!tek_sc_err_success(&parseErr))
	{
		outError = FormatTekError(parseErr);
		return false;
	}

	m_impl->hasManifest = true;
	m_impl->manifest.item_id.app_id = m_ctx.appId;
	m_impl->manifest.item_id.depot_id = m_ctx.depotId;
	m_impl->manifest.id = manifestId;

	const int serSize = tek_sc_dm_serialize(&m_impl->manifest, nullptr, 0);
	if (serSize > 0)
	{
		std::vector<char> serialized(static_cast<size_t>(serSize));
		if (tek_sc_dm_serialize(&m_impl->manifest, serialized.data(), serSize) == 0)
			g_steamCacheManager.StoreManifest(m_ctx.depotId, manifestId, serialized.data(), serialized.size());
	}

	return true;
}

static void BuildFileIndex(const tek_sc_dm_dir* dir, const std::string& prefix,
	std::unordered_map<std::string, const tek_sc_dm_file*>& fileIndex,
	std::unordered_map<std::string, const tek_sc_dm_file*>& basenameIndex)
{
	if (!dir)
		return;

	for (int i = 0; i < dir->num_files; ++i)
	{
		const tek_sc_dm_file& file = dir->files[i];
		const std::string name = CSteamClient::WideToUtf8(file.name);
		std::string full = prefix.empty() ? name : (prefix + "/" + name);
		const std::string norm = CSteamClient::NormalizeDepotPath(full);
		fileIndex[norm] = &file;

		const size_t slash = norm.find_last_of('/');
		const std::string base = (slash == std::string::npos) ? norm : norm.substr(slash + 1);
		basenameIndex.emplace(base, &file); // first wins
	}

	for (int i = 0; i < dir->num_subdirs; ++i)
	{
		const tek_sc_dm_dir& sub = dir->subdirs[i];
		const std::string name = CSteamClient::WideToUtf8(sub.name);
		const std::string next = prefix.empty() ? name : (prefix + "/" + name);
		BuildFileIndex(&sub, next, fileIndex, basenameIndex);
	}
}

bool CSteamClient::EnsureManifest(std::string& outError)
{
	if (m_impl->hasManifest && m_impl->manifest.id == m_ctx.manifestId)
		return true;

	if (!FetchManifest(m_ctx.manifestId, outError))
		return false;

	m_impl->fileIndex.clear();
	m_impl->basenameIndex.clear();
	if (m_impl->manifest.num_dirs > 0)
		BuildFileIndex(&m_impl->manifest.dirs[0], {}, m_impl->fileIndex, m_impl->basenameIndex);

	if (m_impl->manifest.num_chunks <= 0)
	{
		outError = "Depot manifest has no SteamPipe chunks (corrupt cache?). Clear Steam cache and pin again.";
		tek_sc_dm_free(&m_impl->manifest);
		m_impl->hasManifest = false;
		m_impl->fileIndex.clear();
		m_impl->basenameIndex.clear();
		return false;
	}

	Log("STEAM: Manifest %llu — %d files, %d chunks\n",
		static_cast<unsigned long long>(m_ctx.manifestId),
		m_impl->manifest.num_files, m_impl->manifest.num_chunks);

	if (!m_impl->decCtx)
		m_impl->decCtx = tek_sc_sp_dec_ctx_create(m_impl->depotKey);

	return true;
}

void CSteamClient::ClearDepotContext()
{
	m_ctx = {};
	m_impl->hasDepotKey = false;
	if (m_impl->hasManifest)
	{
		tek_sc_dm_free(&m_impl->manifest);
		m_impl->hasManifest = false;
	}
	m_impl->fileIndex.clear();
	m_impl->basenameIndex.clear();
	if (m_impl->decCtx)
	{
		tek_sc_sp_dec_ctx_destroy(m_impl->decCtx);
		m_impl->decCtx = nullptr;
	}
}

namespace
{
	bool FetchProductInfoTree(tek_sc_cm_client* cm, tek_sc_lib_ctx* lib, CallbackWaiter& waiter, uint32_t appId,
		BinaryVdfNode& outRoot, std::string& outError)
	{
		tek_sc_cm_pics_entry appEntry{};
		appEntry.id = appId;

		tek_sc_cm_data_pics tokenReq{};
		tokenReq.app_entries = &appEntry;
		tokenReq.num_app_entries = 1;
		tokenReq.timeout_ms = kDefaultTimeoutMs;

		waiter.Reset();
		waiter.onData = [&](void* data)
			{
				const auto* resp = static_cast<const tek_sc_cm_data_pics*>(data);
				waiter.result = resp->result;
			};
		tek_sc_cm_get_access_token(cm, &tokenReq, &CallbackWaiter::OnCallback, kDefaultTimeoutMs);
		if (!waiter.Wait(kDefaultTimeoutMs, outError))
			return false;

		if (appEntry.access_token != 0)
			tek_sc_lib_add_pics_at(lib, appId, appEntry.access_token);

		tek_sc_cm_data_pics infoReq{};
		infoReq.app_entries = &appEntry;
		infoReq.num_app_entries = 1;
		infoReq.timeout_ms = kDefaultTimeoutMs;

		waiter.Reset();
		waiter.onData = [&](void* data)
			{
				const auto* resp = static_cast<const tek_sc_cm_data_pics*>(data);
				waiter.result = resp->result;
			};
		tek_sc_cm_get_product_info(cm, &infoReq, &CallbackWaiter::OnCallback, kDefaultTimeoutMs);
		if (!waiter.Wait(kDefaultTimeoutMs, outError))
			return false;

		if (!tek_sc_err_success(&waiter.result) || !tek_sc_err_success(&appEntry.result) || !appEntry.data)
		{
			outError = FormatTekError(tek_sc_err_success(&waiter.result) ? appEntry.result : waiter.result);
			if (outError.empty())
				outError = "Failed to get Steam product info for app";
			return false;
		}

		std::unique_ptr<void, TekHeapDeleter> productInfo(appEntry.data);
		appEntry.data = nullptr;

		if (ParseSteamProductInfo(productInfo.get(), static_cast<size_t>(appEntry.data_size), outRoot))
			return true;

		// Text VDF fallback.
		try
		{
			std::string vdfText(static_cast<const char*>(productInfo.get()), static_cast<size_t>(appEntry.data_size));
			auto root = tyti::vdf::read(vdfText.begin(), vdfText.end());
			std::function<void(const tyti::vdf::object&, BinaryVdfNode&)> convert;
			convert = [&](const tyti::vdf::object& src, BinaryVdfNode& dst)
				{
					for (const auto& [k, v] : src.attribs)
					{
						BinaryVdfNode child;
						child.name = k;
						child.stringValue = v;
						child.hasString = true;
						dst.children.emplace_back(std::move(child));
					}
					for (const auto& [k, childPtr] : src.childs)
					{
						if (!childPtr)
							continue;
						BinaryVdfNode child;
						child.name = k;
						convert(*childPtr, child);
						dst.children.emplace_back(std::move(child));
					}
				};
			outRoot = {};
			outRoot.name = "root";
			convert(root, outRoot);
			return !outRoot.children.empty();
		}
		catch (const std::exception& ex)
		{
			outError = std::string("Failed to parse Steam product info: ") + ex.what();
			return false;
		}
	}
} // namespace

bool CSteamClient::QueryAppDepots(uint32_t appId, const std::string& branch,
	std::vector<SteamDepotInfo_t>& outDepots, std::string& outError)
{
	if (!m_signedIn)
	{
		outError = "Sign in to Steam first (QR or account login)";
		return false;
	}
	if (m_anonymous)
	{
		outError = "Anonymous login cannot query/decrypt owned depots. Use QR or account login.";
		return false;
	}
	if (!EnsureLicenses(outError))
		return false;

	const ProductInfoProvider provider = [this](uint32_t id, BinaryVdfNode& root, std::string& err) -> bool
		{
			return FetchProductInfoTree(m_impl->cm, m_impl->lib, m_impl->waiter, id, root, err);
		};
	return BuildDepotList(appId, branch, provider, outDepots, outError);
}

bool CSteamClient::SetDepotContext(uint32_t appId, uint32_t depotId, const std::string& branch,
	uint64_t manifestId, std::string& outError)
{
	if (!m_signedIn)
	{
		outError = "Sign in to Steam first (QR or account login). Depot keys require game ownership.";
		return false;
	}
	if (m_anonymous)
	{
		outError = "Anonymous login cannot decrypt owned depots. Use QR or username/password login.";
		return false;
	}
	if (!EnsureLicenses(outError))
		return false;

	const ProductInfoProvider provider = [this](uint32_t id, BinaryVdfNode& root, std::string& err) -> bool
		{
			return FetchProductInfoTree(m_impl->cm, m_impl->lib, m_impl->waiter, id, root, err);
		};

	ResolvedDepot_t resolved;
	if (!ResolveDepotTarget(appId, depotId, branch, manifestId, provider, resolved, outError))
		return false;

	const SteamDepotContext_t previous = m_ctx;
	m_ctx.appId = appId;
	m_ctx.depotId = depotId;
	m_ctx.keyAppId = resolved.keyAppId;
	m_ctx.branch = branch.empty() ? "public" : branch;
	m_ctx.manifestId = resolved.manifestId;
	m_impl->hasDepotKey = false;
	if (m_impl->decCtx)
	{
		tek_sc_sp_dec_ctx_destroy(m_impl->decCtx);
		m_impl->decCtx = nullptr;
	}

	if (!EnsureManifest(outError))
	{
		m_ctx = previous;
		return false;
	}

	return true;
}

bool CSteamClient::ListManifestFiles(std::vector<SteamDepotFile_t>& outFiles, std::string& outError)
{
	if (!EnsureManifest(outError))
		return false;

	outFiles.clear();
	outFiles.reserve(m_impl->fileIndex.size());
	for (const auto& [path, file] : m_impl->fileIndex)
	{
		SteamDepotFile_t entry;
		entry.depotPath = path;
		entry.size = file->size;
		outFiles.emplace_back(std::move(entry));
	}
	std::sort(outFiles.begin(), outFiles.end(), [](const SteamDepotFile_t& a, const SteamDepotFile_t& b)
		{
			return a.depotPath < b.depotPath;
		});
	return true;
}

bool CSteamClient::FindManifestFile(const std::string& depotPathOrName, SteamDepotFile_t& outFile, std::string& outError)
{
	if (!EnsureManifest(outError))
		return false;

	const std::string norm = NormalizeDepotPath(depotPathOrName);
	auto it = m_impl->fileIndex.find(norm);
	if (it == m_impl->fileIndex.end())
	{
		const size_t slash = norm.find_last_of('/');
		const std::string base = (slash == std::string::npos) ? norm : norm.substr(slash + 1);
		auto bit = m_impl->basenameIndex.find(base);
		if (bit == m_impl->basenameIndex.end())
		{
			outError = "File not found in depot manifest: " + depotPathOrName;
			return false;
		}
		it = m_impl->fileIndex.end();
		for (auto fit = m_impl->fileIndex.begin(); fit != m_impl->fileIndex.end(); ++fit)
		{
			if (fit->second == bit->second)
			{
				it = fit;
				break;
			}
		}
		if (it == m_impl->fileIndex.end())
		{
			outError = "File not found in depot manifest: " + depotPathOrName;
			return false;
		}
	}

	outFile.depotPath = it->first;
	outFile.size = it->second->size;
	return true;
}

bool CSteamClient::GetOverlappingChunks(const std::string& depotPath, uint64_t offset, uint64_t size,
	std::vector<SteamChunkRef_t>& outChunks, std::string& outError)
{
	if (!EnsureManifest(outError))
		return false;

	SteamDepotFile_t file{};
	if (!FindManifestFile(depotPath, file, outError))
		return false;

	const auto it = m_impl->fileIndex.find(file.depotPath);
	if (it == m_impl->fileIndex.end())
	{
		outError = "Internal: file index miss";
		return false;
	}

	const tek_sc_dm_file* dmFile = it->second;
	if (!dmFile->chunks || dmFile->num_chunks <= 0)
	{
		outError = "File has no SteamPipe chunks in manifest: " + file.depotPath
			+ " (size=" + std::to_string(dmFile->size) + " flags=" + std::to_string(dmFile->flags) + ")";
		return false;
	}

	const uint64_t end = offset + size;
	outChunks.clear();

	for (int i = 0; i < dmFile->num_chunks; ++i)
	{
		const tek_sc_dm_chunk& chunk = dmFile->chunks[i];
		const uint64_t chunkEnd = static_cast<uint64_t>(chunk.offset) + static_cast<uint64_t>(chunk.size);
		if (chunkEnd <= offset || static_cast<uint64_t>(chunk.offset) >= end)
			continue;

		SteamChunkRef_t ref;
		ref.sha1Hex = Sha1ToHex(chunk.sha.bytes);
		ref.offset = chunk.offset;
		ref.size = chunk.size;
		ref.compSize = chunk.comp_size;
		outChunks.emplace_back(std::move(ref));
	}

	return true;
}

bool CSteamClient::DownloadAndDecodeChunk(const SteamChunkRef_t& chunk, std::vector<char>& outDecoded, std::string& outError)
{
	if (g_steamCacheManager.TryGetChunk(m_ctx.depotId, chunk.sha1Hex, outDecoded))
		return true;

	if (!EnsureServers(outError) || !EnsureDepotKey(outError))
		return false;

	if (!m_impl->decCtx)
		m_impl->decCtx = tek_sc_sp_dec_ctx_create(m_impl->depotKey);

	// Deduplicate concurrent downloads of the same chunk.
	{
		std::unique_lock lock(m_impl->chunkMutex);
		if (m_impl->chunkInFlight.count(chunk.sha1Hex))
		{
			auto cv = m_impl->chunkWaiters[chunk.sha1Hex];
			if (!cv)
			{
				cv = std::make_shared<std::condition_variable>();
				m_impl->chunkWaiters[chunk.sha1Hex] = cv;
			}
			cv->wait(lock, [&] { return !m_impl->chunkInFlight.count(chunk.sha1Hex); });
			if (g_steamCacheManager.TryGetChunk(m_ctx.depotId, chunk.sha1Hex, outDecoded))
				return true;
			outError = "Chunk download failed in another thread";
			return false;
		}
		m_impl->chunkInFlight.insert(chunk.sha1Hex);
		if (!m_impl->chunkWaiters.count(chunk.sha1Hex))
			m_impl->chunkWaiters[chunk.sha1Hex] = std::make_shared<std::condition_variable>();
	}

	struct InFlightGuard
	{
		Impl* impl;
		std::string sha;
		~InFlightGuard()
		{
			std::lock_guard lock(impl->chunkMutex);
			impl->chunkInFlight.erase(sha);
			if (auto it = impl->chunkWaiters.find(sha); it != impl->chunkWaiters.end())
				it->second->notify_all();
		}
	} guard{ m_impl.get(), chunk.sha1Hex };

	// Rebuild a temporary tek_sc_dm_chunk for the download API.
	tek_sc_dm_chunk dmChunk{};
	for (int i = 0; i < 20; ++i)
	{
		const char c1 = chunk.sha1Hex[i * 2];
		const char c2 = chunk.sha1Hex[i * 2 + 1];
		auto hex = [](char c) -> unsigned char
			{
				if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
				if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
				if (c >= 'A' && c <= 'F') return static_cast<unsigned char>(c - 'A' + 10);
				return 0;
			};
		dmChunk.sha.bytes[i] = static_cast<unsigned char>((hex(c1) << 4) | hex(c2));
	}
	dmChunk.offset = chunk.offset;
	dmChunk.size = chunk.size;
	dmChunk.comp_size = chunk.compSize;

	std::vector<char> compressed(static_cast<size_t>(chunk.compSize));
	tek_sc_sp_data_chunk data{};
	data.data = compressed.data();
	data.depot_id = m_ctx.depotId;
	data.chunk = &dmChunk;
	data.cm_client = m_impl->cm;

	std::atomic_bool cancel{ false };
	tek_sc_err dlErr{};
	for (auto& srv : m_impl->servers)
	{
		dlErr = tek_sc_sp_download_chunk(&srv, &data, kDefaultTimeoutMs, &cancel);
		if (tek_sc_err_success(&dlErr))
			break;
	}

	if (!tek_sc_err_success(&dlErr))
	{
		outError = FormatTekError(dlErr);
		if (outError.empty())
			outError = "Failed to download Steam chunk " + chunk.sha1Hex;
		return false;
	}

	outDecoded.resize(static_cast<size_t>(chunk.size));
	const tek_sc_err decErr = tek_sc_sp_decode_chunk(m_impl->decCtx, compressed.data(), outDecoded.data(), &dmChunk);
	if (!tek_sc_err_success(&decErr))
	{
		outError = FormatTekError(decErr);
		return false;
	}

	g_steamCacheManager.StoreChunk(m_ctx.depotId, chunk.sha1Hex, outDecoded.data(), outDecoded.size());
	return true;
}

bool CSteamClient::ReadFileRange(const std::string& depotPath, uint64_t offset, uint64_t size,
	std::vector<char>& out, std::string& outError)
{
	if (size == 0)
	{
		out.clear();
		return true;
	}

	std::vector<SteamChunkRef_t> chunks;
	if (!GetOverlappingChunks(depotPath, offset, size, chunks, outError))
		return false;

	if (chunks.empty())
	{
		outError = "Manifest lists no SteamPipe chunks for " + depotPath
			+ " (size " + std::to_string(size) + "). Re-pin the depot / clear Steam cache.";
		return false;
	}

	out.assign(static_cast<size_t>(size), '\0');
	std::vector<char> covered(static_cast<size_t>(size), 0);
	for (const SteamChunkRef_t& chunk : chunks)
	{
		std::vector<char> decoded;
		if (!DownloadAndDecodeChunk(chunk, decoded, outError))
			return false;

		const uint64_t chunkStart = static_cast<uint64_t>(chunk.offset);
		const uint64_t copyStart = (std::max)(chunkStart, offset);
		const uint64_t copyEnd = (std::min)(chunkStart + static_cast<uint64_t>(chunk.size), offset + size);
		if (copyEnd <= copyStart)
			continue;

		const size_t dstOff = static_cast<size_t>(copyStart - offset);
		const size_t srcOff = static_cast<size_t>(copyStart - chunkStart);
		const size_t copySize = static_cast<size_t>(copyEnd - copyStart);
		if (srcOff + copySize > decoded.size())
		{
			outError = "Decoded chunk shorter than manifest size for " + chunk.sha1Hex;
			return false;
		}
		memcpy(out.data() + dstOff, decoded.data() + srcOff, copySize);
		memset(covered.data() + dstOff, 1, copySize);
	}

	if (std::find(covered.begin(), covered.end(), 0) != covered.end())
	{
		outError = "Incomplete SteamPipe coverage for " + depotPath;
		return false;
	}

	return true;
}

bool CSteamClient::DownloadFile(const std::string& depotPath, std::vector<char>& out, std::string& outError)
{
	SteamDepotFile_t file{};
	if (!FindManifestFile(depotPath, file, outError))
		return false;

	if (!EnsureManifest(outError))
		return false;

	const auto it = m_impl->fileIndex.find(file.depotPath);
	if (it != m_impl->fileIndex.end() && it->second
		&& (it->second->flags & TEK_SC_DM_FILE_FLAG_symlink) && it->second->target_path)
	{
		const std::string target = NormalizeDepotPath(WideToUtf8(it->second->target_path));
		return ReadFileRange(target, 0, static_cast<uint64_t>(file.size), out, outError);
	}

	return ReadFileRange(file.depotPath, 0, static_cast<uint64_t>(file.size), out, outError);
}

bool CSteamClient::DownloadFileToPath(const std::string& depotPath, const std::filesystem::path& destPath, std::string& outError)
{
	std::vector<char> data;
	if (!DownloadFile(depotPath, data, outError))
		return false;

	std::error_code ec;
	std::filesystem::create_directories(destPath.parent_path(), ec);
	StreamIO file;
	if (!file.open(destPath.string(), eStreamIOMode::Write))
	{
		outError = "Failed to write " + destPath.string();
		return false;
	}
	if (!data.empty())
		file.write(data.data(), data.size());
	return true;
}
