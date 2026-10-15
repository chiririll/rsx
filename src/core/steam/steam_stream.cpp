#include <pch.h>
#include <core/steam/steam_stream.h>
#include <core/steam/steamclient.h>
#include <core/steam/steamcache.h>

namespace
{
	struct VirtualEntry_t
	{
		std::string depotPath;
		uint64_t size;
	};

	// Normalized depot path -> entry, rebuilt when the pinned manifest changes.
	std::mutex s_indexMutex;
	uint64_t s_indexManifestId = 0;
	using VirtualIndex_t = std::unordered_map<std::string, VirtualEntry_t>;
	std::shared_ptr<const VirtualIndex_t> s_index;

	// ReadFileRange isn't known to be reentrant, and decoding may happen on several threads.
	std::mutex s_readMutex;

	// Index for the current depot context. Returns false if nothing is pinned.
	bool GetIndex(std::filesystem::path& outRoot, std::shared_ptr<const VirtualIndex_t>& outIndex)
	{
		if (!g_steamClient.HasDepotContext())
			return false;

		const auto& ctx = g_steamClient.GetDepotContext();
		outRoot = std::filesystem::absolute(g_steamCacheManager.GetRPakCacheRoot(ctx.depotId, ctx.manifestId)).lexically_normal();

		std::lock_guard lock(s_indexMutex);
		if (s_indexManifestId != ctx.manifestId)
		{
			std::vector<SteamDepotFile_t> files;
			std::string error;
			if (!g_steamClient.ListManifestFiles(files, error))
				return false;

			auto index = std::make_shared<VirtualIndex_t>();
			for (const auto& file : files)
				(*index)[CSteamClient::NormalizeDepotPath(file.depotPath)] = { file.depotPath, static_cast<uint64_t>(file.size) };
			s_index = std::move(index);
			s_indexManifestId = ctx.manifestId;
		}

		outIndex = s_index;
		return true;
	}

	// Cache path -> normalized depot-relative path, or empty if it isn't under root.
	std::string RelativeDepotPath(const std::filesystem::path& root, const std::filesystem::path& path)
	{
		const std::filesystem::path rel = std::filesystem::absolute(path).lexically_normal().lexically_relative(root);
		if (rel.empty() || *rel.begin() == "..")
			return {};
		return CSteamClient::NormalizeDepotPath(rel.generic_string());
	}

	// Read-only streambuf over a depot file, fetched in aligned blocks with a small LRU.
	class CSteamRangeBuf final : public std::streambuf
	{
	public:
		CSteamRangeBuf(std::string depotPath, uint64_t size) : m_depotPath(std::move(depotPath)), m_size(size) {}

	protected:
		int_type underflow() override
		{
			if (gptr() < egptr())
				return traits_type::to_int_type(*gptr());

			const uint64_t pos = Tell();
			if (pos >= m_size || !LoadBlock(pos))
				return traits_type::eof();

			return traits_type::to_int_type(*gptr());
		}

		pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override
		{
			if (!(which & std::ios_base::in))
				return pos_type(off_type(-1));

			int64_t base = 0;
			switch (dir)
			{
			case std::ios_base::beg: base = 0; break;
			case std::ios_base::cur: base = static_cast<int64_t>(Tell()); break;
			case std::ios_base::end: base = static_cast<int64_t>(m_size); break;
			default: return pos_type(off_type(-1));
			}

			return SeekTo(base + off);
		}

		pos_type seekpos(pos_type pos, std::ios_base::openmode which) override
		{
			return seekoff(off_type(pos), std::ios_base::beg, which);
		}

	private:
		static constexpr uint64_t kBlockSize = 1024 * 1024;
		static constexpr size_t kMaxBlocks = 8;

		struct Block_t
		{
			uint64_t index;
			std::vector<char> data;
		};

		uint64_t Tell() const
		{
			return eback() ? m_blockStart + static_cast<uint64_t>(gptr() - eback()) : m_pos;
		}

		pos_type SeekTo(int64_t target)
		{
			if (target < 0 || static_cast<uint64_t>(target) > m_size)
				return pos_type(off_type(-1));

			const uint64_t t = static_cast<uint64_t>(target);
			if (eback() && t >= m_blockStart && t < m_blockStart + static_cast<uint64_t>(egptr() - eback()))
			{
				setg(eback(), eback() + (t - m_blockStart), egptr());
			}
			else
			{
				setg(nullptr, nullptr, nullptr);
				m_pos = t;
			}

			return pos_type(off_type(target));
		}

		bool LoadBlock(uint64_t pos)
		{
			const uint64_t index = pos / kBlockSize;
			const uint64_t start = index * kBlockSize;

			auto it = std::find_if(m_blocks.begin(), m_blocks.end(), [index](const Block_t& b) { return b.index == index; });
			if (it == m_blocks.end())
			{
				const uint64_t len = std::min(kBlockSize, m_size - start);

				Block_t block{ index, {} };
				std::string error;
				{
					std::lock_guard lock(s_readMutex);
					if (!g_steamClient.ReadFileRange(m_depotPath, start, len, block.data, error) || block.data.size() != len)
					{
						Log("STEAM: range read of %s failed: %s\n", m_depotPath.c_str(), error.c_str());
						return false;
					}
				}

				m_blocks.push_front(std::move(block));
				if (m_blocks.size() > kMaxBlocks)
					m_blocks.pop_back();
				it = m_blocks.begin();
			}
			else if (it != m_blocks.begin())
			{
				m_blocks.splice(m_blocks.begin(), m_blocks, it);
				it = m_blocks.begin();
			}

			char* const data = it->data.data();
			m_blockStart = start;
			setg(data, data + (pos - start), data + it->data.size());
			return true;
		}

		std::string m_depotPath;
		uint64_t m_size;
		uint64_t m_pos = 0;        // valid while no block is active
		uint64_t m_blockStart = 0; // valid while a block is active
		std::list<Block_t> m_blocks;
	};
}

std::unique_ptr<std::streambuf> SteamOpenVirtualFile(const std::filesystem::path& path)
{
	std::error_code ec;
	if (std::filesystem::exists(path, ec))
		return nullptr;

	std::filesystem::path root;
	std::shared_ptr<const VirtualIndex_t> index;
	if (!GetIndex(root, index))
		return nullptr;

	const std::string rel = RelativeDepotPath(root, path);
	if (rel.empty())
		return nullptr;

	const auto it = index->find(rel);
	if (it == index->end())
		return nullptr;

	return std::make_unique<CSteamRangeBuf>(it->second.depotPath, it->second.size);
}

void SteamListVirtualFiles(const std::filesystem::path& dir, const char* ext, std::vector<std::filesystem::path>& out)
{
	std::filesystem::path root;
	std::shared_ptr<const VirtualIndex_t> index;
	if (!GetIndex(root, index))
		return;

	std::string prefix = RelativeDepotPath(root, dir);
	if (!prefix.empty())
		prefix += '/';
	else if (std::filesystem::absolute(dir).lexically_normal() != root)
		return; // not under the cache root at all

	for (const auto& [norm, entry] : *index)
	{
		if (norm.size() <= prefix.size() || norm.compare(0, prefix.size(), prefix) != 0)
			continue;
		if (norm.find('/', prefix.size()) != std::string::npos)
			continue;
		if (std::filesystem::path(norm).extension() != ext)
			continue;

		const std::filesystem::path local = root / std::filesystem::path(entry.depotPath);
		std::error_code ec;
		if (!std::filesystem::exists(local, ec))
			out.push_back(local);
	}
}
