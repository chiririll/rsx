#pragma once

#include <atomic>
#include <condition_variable>
#include <shared_mutex>

struct SteamDepotFile_t
{
	std::string depotPath; // depot-relative path using '/' separators
	int64_t size = 0;
};

struct SteamDepotContext_t
{
	uint32_t appId = 0;
	uint32_t depotId = 0;
	// App ID used for GetDepotDecryptionKey (may differ when depotfromapp is set).
	uint32_t keyAppId = 0;
	uint64_t manifestId = 0;
	std::string branch = "public";
};

struct SteamChunkRef_t
{
	std::string sha1Hex;
	int64_t offset = 0;
	int size = 0;
	int compSize = 0;
};

struct SteamDepotInfo_t
{
	uint32_t depotId = 0;
	std::string name;
	uint64_t branchManifestId = 0; // manifest for the requested branch (usually public)
	uint32_t depotFromApp = 0; // shared depot parent app, if any
	std::string oslist;
};

// Thin C++ wrapper around tek-steamclient CM + SteamPipe APIs.
class CSteamClient
{
public:
	using GuardCodeCallback = std::function<std::string(const std::string& prompt)>;
	// Called on the CM callback thread whenever Steam issues a (new) QR login URL.
	using QrUrlCallback = std::function<void(const std::string& url)>;

	CSteamClient();
	~CSteamClient();

	CSteamClient(const CSteamClient&) = delete;
	CSteamClient& operator=(const CSteamClient&) = delete;

	bool Init(std::string& outError);
	void Shutdown();

	bool IsConnected() const { return m_connected; }
	bool IsSignedIn() const { return m_signedIn; }
	bool IsAnonymous() const { return m_anonymous; }
	const std::string& GetUsername() const { return m_username; }

	// Full login. If a remembered token exists and rememberLogin is true, tries token first.
	bool Login(const std::string& username, const std::string& password, bool rememberLogin,
		const GuardCodeCallback& guardCb, std::string& outError);

	// QR login via Steam mobile app. onUrl receives the URL to encode as a QR code
	// (may be called multiple times if Steam refreshes it). cancelFlag may be set
	// from another thread to abort waiting.
	bool LoginWithQr(bool rememberLogin, const QrUrlCallback& onUrl,
		std::atomic<bool>* cancelFlag, std::string& outError);

	bool LoginAnonymous(std::string& outError);
	bool LoginWithToken(const std::string& token, std::string& outError);

	// Connect + sign in using the remembered refresh token (no UI). Tries renew on failure.
	bool TryRestoreSession(std::string& outError);

	void Logout();

	bool HasRememberedToken() const;
	std::string GetRememberedUsername() const;

	// List depots (+ branch manifests) from PICS product info for an app.
	bool QueryAppDepots(uint32_t appId, const std::string& branch,
		std::vector<SteamDepotInfo_t>& outDepots, std::string& outError);

	// Pin a depot + explicit manifest. Manifest ID 0 means "latest for branch".
	// Requires an authenticated (non-anonymous) session for depot keys.
	bool SetDepotContext(uint32_t appId, uint32_t depotId, const std::string& branch,
		uint64_t manifestId, std::string& outError);
	void ClearDepotContext();
	const SteamDepotContext_t& GetDepotContext() const { return m_ctx; }
	bool HasDepotContext() const { return m_ctx.appId != 0 && m_ctx.depotId != 0 && m_ctx.manifestId != 0; }

	// Enumerate files in the pinned manifest.
	bool ListManifestFiles(std::vector<SteamDepotFile_t>& outFiles, std::string& outError);

	// Find a file in the pinned manifest by full path or case-insensitive basename.
	bool FindManifestFile(const std::string& depotPathOrName, SteamDepotFile_t& outFile, std::string& outError);

	// Download a byte range from a depot file (assembles overlapping chunks).
	bool ReadFileRange(const std::string& depotPath, uint64_t offset, uint64_t size,
		std::vector<char>& out, std::string& outError);

	// Download an entire small file (rpak) into out / or to a local path.
	bool DownloadFile(const std::string& depotPath, std::vector<char>& out, std::string& outError);
	bool DownloadFileToPath(const std::string& depotPath, const std::filesystem::path& destPath, std::string& outError);

	// Resolve chunks overlapping [offset, offset+size).
	bool GetOverlappingChunks(const std::string& depotPath, uint64_t offset, uint64_t size,
		std::vector<SteamChunkRef_t>& outChunks, std::string& outError);

	static std::string WideToUtf8(const wchar_t* wstr);
	static std::string NormalizeDepotPath(const std::string& path);

private:
	struct Impl;

	bool EnsureConnected(std::string& outError);
	bool EnsureLicenses(std::string& outError);
	bool EnsureManifest(std::string& outError);
	bool EnsureDepotKey(std::string& outError);
	bool EnsureServers(std::string& outError);

	bool FetchManifest(uint64_t manifestId, std::string& outError);
	bool DownloadAndDecodeChunk(const SteamChunkRef_t& chunk, std::vector<char>& outDecoded, std::string& outError);

	static std::string Sha1ToHex(const unsigned char sha[20]);
	static std::wstring Utf8ToWide(const std::string& str);
	static std::filesystem::path TokenFilePath();

	bool SaveToken(const std::string& username, const std::string& token) const;
	bool LoadToken(std::string& username, std::string& token) const;
	void ClearToken() const;

	std::unique_ptr<Impl> m_impl;
	SteamDepotContext_t m_ctx;

	bool m_connected = false;
	bool m_signedIn = false;
	bool m_anonymous = false;
	std::string m_username;
};

extern CSteamClient g_steamClient;
