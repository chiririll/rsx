#pragma once

#include <shared_mutex>

// On-disk cache for Steam depot manifests, decryption keys, decoded chunks,
// and parsed starpak TOCs. Chunks are content-addressed by SHA1 so they are
// reused across manifest versions automatically.
class CSteamCacheManager
{
public:
	static constexpr uint64_t DEFAULT_MAX_CACHE_BYTES = 8ull * 1024ull * 1024ull * 1024ull; // 8 GiB

	void SetCacheDirectory(const std::filesystem::path& path);
	const std::filesystem::path& GetCacheDirectory() const { return m_cacheDir; }

	void SetMaxCacheBytes(uint64_t bytes) { m_maxCacheBytes = bytes; }
	uint64_t GetMaxCacheBytes() const { return m_maxCacheBytes; }

	// Depot AES-256 key (32 bytes).
	bool TryGetDepotKey(uint32_t depotId, unsigned char outKey[32]) const;
	bool StoreDepotKey(uint32_t depotId, const unsigned char key[32]);

	// Serialized tek_sc_depot_manifest blob.
	bool TryGetManifest(uint32_t depotId, uint64_t manifestId, std::vector<char>& out) const;
	bool StoreManifest(uint32_t depotId, uint64_t manifestId, const void* data, size_t size);

	// Decoded (plaintext) chunk payload, keyed by SHA1 hex.
	bool TryGetChunk(uint32_t depotId, const std::string& sha1Hex, std::vector<char>& out) const;
	bool StoreChunk(uint32_t depotId, const std::string& sha1Hex, const void* data, size_t size);

	// Parsed starpak TOC: sequence of {uint64 offset, uint64 size} pairs.
	bool TryGetStarPakToc(uint32_t depotId, uint64_t manifestId, const std::string& fileKey,
		std::unordered_map<uint64_t, size_t>& out) const;
	bool StoreStarPakToc(uint32_t depotId, uint64_t manifestId, const std::string& fileKey,
		const std::unordered_map<uint64_t, size_t>& toc);

	// Materialized depot files (rpaks) live under rpak/<depotId>_<manifestId>/...
	std::filesystem::path GetRPakCacheRoot(uint32_t depotId, uint64_t manifestId) const;

	void Clear();
	void EnforceSizeCap();

private:
	std::filesystem::path ChunkPath(uint32_t depotId, const std::string& sha1Hex) const;
	std::filesystem::path ManifestPath(uint32_t depotId, uint64_t manifestId) const;
	std::filesystem::path KeyPath(uint32_t depotId) const;
	std::filesystem::path TocPath(uint32_t depotId, uint64_t manifestId, const std::string& fileKey) const;

	static std::string SanitizeFileKey(const std::string& fileKey);
	static bool ReadFileBytes(const std::filesystem::path& path, std::vector<char>& out);
	static bool WriteFileBytes(const std::filesystem::path& path, const void* data, size_t size);
	void TouchFile(const std::filesystem::path& path) const;

	std::filesystem::path m_cacheDir;
	uint64_t m_maxCacheBytes = DEFAULT_MAX_CACHE_BYTES;
	mutable std::shared_mutex m_mutex;
};

extern CSteamCacheManager g_steamCacheManager;
