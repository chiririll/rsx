#include <pch.h>
#include <game/rtech/steam_starpak_source.h>
#include <core/steam/steamclient.h>

CSteamStarPakSource::CSteamStarPakSource(std::string depotPath, uint64_t fileSize)
	: m_depotPath(std::move(depotPath))
	, m_size(fileSize)
{
}

uint64_t CSteamStarPakSource::size() const
{
	return m_size;
}

std::unique_ptr<char[]> CSteamStarPakSource::readAt(uint64_t offset, uint64_t size) const
{
	if (size == 0 || offset + size > m_size)
		return nullptr;

	std::vector<char> data;
	std::string error;
	if (!g_steamClient.ReadFileRange(m_depotPath, offset, size, data, error))
	{
		Log("STEAM: Failed to read %s [%llu+%llu]: %s\n", m_depotPath.c_str(),
			static_cast<unsigned long long>(offset), static_cast<unsigned long long>(size), error.c_str());
		return nullptr;
	}

	std::unique_ptr<char[]> out(new char[size]);
	memcpy(out.get(), data.data(), static_cast<size_t>(size));
	return out;
}
