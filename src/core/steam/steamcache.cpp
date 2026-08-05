#include <pch.h>
#include <shared_mutex>
#include <core/steam/steamcache.h>

CSteamCacheManager g_steamCacheManager;

void CSteamCacheManager::SetCacheDirectory(const std::filesystem::path& path)
{
	std::unique_lock lock(m_mutex);
	m_cacheDir = path;
	std::error_code ec;
	std::filesystem::create_directories(m_cacheDir / "chunks", ec);
	std::filesystem::create_directories(m_cacheDir / "manifests", ec);
	std::filesystem::create_directories(m_cacheDir / "keys", ec);
	std::filesystem::create_directories(m_cacheDir / "toc", ec);
	std::filesystem::create_directories(m_cacheDir / "rpak", ec);
}

std::string CSteamCacheManager::SanitizeFileKey(const std::string& fileKey)
{
	std::string out;
	out.reserve(fileKey.size());
	for (const char c : fileKey)
	{
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
			out.push_back(c);
		else
			out.push_back('_');
	}
	return out;
}

bool CSteamCacheManager::ReadFileBytes(const std::filesystem::path& path, std::vector<char>& out)
{
	std::error_code ec;
	if (!std::filesystem::exists(path, ec))
		return false;

	StreamIO file;
	if (!file.open(path.string(), eStreamIOMode::Read))
		return false;

	const size_t size = file.size();
	out.resize(size);
	if (size > 0)
		file.read(out.data(), size);
	return true;
}

bool CSteamCacheManager::WriteFileBytes(const std::filesystem::path& path, const void* data, size_t size)
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);

	StreamIO file;
	if (!file.open(path.string(), eStreamIOMode::Write))
		return false;

	if (size > 0)
		file.write(static_cast<const char*>(data), size);
	return true;
}

void CSteamCacheManager::TouchFile(const std::filesystem::path& path) const
{
	const auto now = std::filesystem::file_time_type::clock::now();
	std::error_code ec;
	std::filesystem::last_write_time(path, now, ec);
}

std::filesystem::path CSteamCacheManager::ChunkPath(uint32_t depotId, const std::string& sha1Hex) const
{
	return m_cacheDir / "chunks" / std::to_string(depotId) / (sha1Hex + ".bin");
}

std::filesystem::path CSteamCacheManager::ManifestPath(uint32_t depotId, uint64_t manifestId) const
{
	return m_cacheDir / "manifests" / std::format("{}_{}.bin", depotId, manifestId);
}

std::filesystem::path CSteamCacheManager::KeyPath(uint32_t depotId) const
{
	return m_cacheDir / "keys" / (std::to_string(depotId) + ".key");
}

std::filesystem::path CSteamCacheManager::TocPath(uint32_t depotId, uint64_t manifestId, const std::string& fileKey) const
{
	return m_cacheDir / "toc" / std::format("{}_{}_{}.bin", depotId, manifestId, SanitizeFileKey(fileKey));
}

std::filesystem::path CSteamCacheManager::GetRPakCacheRoot(uint32_t depotId, uint64_t manifestId) const
{
	return m_cacheDir / "rpak" / std::format("{}_{}", depotId, manifestId);
}

bool CSteamCacheManager::TryGetDepotKey(uint32_t depotId, unsigned char outKey[32]) const
{
	std::shared_lock lock(m_mutex);
	std::vector<char> bytes;
	if (!ReadFileBytes(KeyPath(depotId), bytes) || bytes.size() != 32)
		return false;
	memcpy(outKey, bytes.data(), 32);
	TouchFile(KeyPath(depotId));
	return true;
}

bool CSteamCacheManager::StoreDepotKey(uint32_t depotId, const unsigned char key[32])
{
	std::unique_lock lock(m_mutex);
	const bool ok = WriteFileBytes(KeyPath(depotId), key, 32);
	if (ok)
		EnforceSizeCap();
	return ok;
}

bool CSteamCacheManager::TryGetManifest(uint32_t depotId, uint64_t manifestId, std::vector<char>& out) const
{
	std::shared_lock lock(m_mutex);
	if (!ReadFileBytes(ManifestPath(depotId, manifestId), out))
		return false;
	TouchFile(ManifestPath(depotId, manifestId));
	return true;
}

bool CSteamCacheManager::StoreManifest(uint32_t depotId, uint64_t manifestId, const void* data, size_t size)
{
	std::unique_lock lock(m_mutex);
	const bool ok = WriteFileBytes(ManifestPath(depotId, manifestId), data, size);
	if (ok)
		EnforceSizeCap();
	return ok;
}

bool CSteamCacheManager::TryGetChunk(uint32_t depotId, const std::string& sha1Hex, std::vector<char>& out) const
{
	std::shared_lock lock(m_mutex);
	const auto path = ChunkPath(depotId, sha1Hex);
	if (!ReadFileBytes(path, out))
		return false;
	TouchFile(path);
	return true;
}

bool CSteamCacheManager::StoreChunk(uint32_t depotId, const std::string& sha1Hex, const void* data, size_t size)
{
	std::unique_lock lock(m_mutex);
	const bool ok = WriteFileBytes(ChunkPath(depotId, sha1Hex), data, size);
	if (ok)
		EnforceSizeCap();
	return ok;
}

bool CSteamCacheManager::TryGetStarPakToc(uint32_t depotId, uint64_t manifestId, const std::string& fileKey,
	std::unordered_map<uint64_t, size_t>& out) const
{
	std::shared_lock lock(m_mutex);
	std::vector<char> bytes;
	const auto path = TocPath(depotId, manifestId, fileKey);
	if (!ReadFileBytes(path, bytes) || (bytes.size() % 16) != 0)
		return false;

	out.clear();
	for (size_t i = 0; i + 16 <= bytes.size(); i += 16)
	{
		uint64_t offset = 0;
		uint64_t size = 0;
		memcpy(&offset, bytes.data() + i, sizeof(offset));
		memcpy(&size, bytes.data() + i + 8, sizeof(size));
		if (size != 0)
			out.emplace(offset, static_cast<size_t>(size));
	}

	TouchFile(path);
	return true;
}

bool CSteamCacheManager::StoreStarPakToc(uint32_t depotId, uint64_t manifestId, const std::string& fileKey,
	const std::unordered_map<uint64_t, size_t>& toc)
{
	std::vector<char> bytes(toc.size() * 16);
	size_t i = 0;
	for (const auto& [offset, size] : toc)
	{
		const uint64_t off = offset;
		const uint64_t sz = static_cast<uint64_t>(size);
		memcpy(bytes.data() + i, &off, sizeof(off));
		memcpy(bytes.data() + i + 8, &sz, sizeof(sz));
		i += 16;
	}

	std::unique_lock lock(m_mutex);
	const bool ok = WriteFileBytes(TocPath(depotId, manifestId, fileKey), bytes.data(), bytes.size());
	if (ok)
		EnforceSizeCap();
	return ok;
}

void CSteamCacheManager::Clear()
{
	std::unique_lock lock(m_mutex);
	std::error_code ec;
	std::filesystem::remove_all(m_cacheDir, ec);
	std::filesystem::create_directories(m_cacheDir / "chunks", ec);
	std::filesystem::create_directories(m_cacheDir / "manifests", ec);
	std::filesystem::create_directories(m_cacheDir / "keys", ec);
	std::filesystem::create_directories(m_cacheDir / "toc", ec);
	std::filesystem::create_directories(m_cacheDir / "rpak", ec);
}

void CSteamCacheManager::EnforceSizeCap()
{
	// Caller must hold exclusive lock.
	if (m_cacheDir.empty() || m_maxCacheBytes == 0)
		return;

	struct FileEntry
	{
		std::filesystem::path path;
		uint64_t size = 0;
		std::filesystem::file_time_type mtime{};
	};

	std::vector<FileEntry> files;
	uint64_t total = 0;
	std::error_code ec;

	for (std::filesystem::recursive_directory_iterator it(m_cacheDir, ec), end; it != end && !ec; it.increment(ec))
	{
		if (!it->is_regular_file(ec))
			continue;

		FileEntry entry;
		entry.path = it->path();
		entry.size = static_cast<uint64_t>(it->file_size(ec));
		entry.mtime = it->last_write_time(ec);
		total += entry.size;
		files.emplace_back(std::move(entry));
	}

	if (total <= m_maxCacheBytes)
		return;

	std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b)
		{
			return a.mtime < b.mtime;
		});

	for (const FileEntry& entry : files)
	{
		if (total <= m_maxCacheBytes)
			break;

		// Keep depot keys - they're tiny and expensive to re-fetch.
		if (entry.path.parent_path().filename() == "keys")
			continue;

		std::filesystem::remove(entry.path, ec);
		if (!ec)
			total -= entry.size;
	}
}
