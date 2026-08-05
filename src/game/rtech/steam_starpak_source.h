#pragma once

#include <game/rtech/starpak_source.h>

// StarPak backend that reads byte ranges from a pinned Steam depot manifest,
// downloading and caching only the overlapping chunks.
class CSteamStarPakSource : public CStarPakSource
{
public:
	CSteamStarPakSource(std::string depotPath, uint64_t fileSize);

	uint64_t size() const override;
	std::unique_ptr<char[]> readAt(uint64_t offset, uint64_t size) const override;

	const std::string& depotPath() const { return m_depotPath; }

private:
	std::string m_depotPath;
	uint64_t m_size = 0;
};
