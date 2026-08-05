#include <pch.h>
#include <game/rtech/starpak_source.h>

CLocalStarPakSource::CLocalStarPakSource(std::filesystem::path path)
	: m_path(std::move(path))
{
	std::error_code ec;
	const auto fileSize = std::filesystem::file_size(m_path, ec);
	m_size = ec ? 0ull : static_cast<uint64_t>(fileSize);
}

uint64_t CLocalStarPakSource::size() const
{
	return m_size;
}

std::unique_ptr<char[]> CLocalStarPakSource::readAt(uint64_t offset, uint64_t size) const
{
	if (size == 0 || offset + size > m_size)
		return nullptr;

	StreamIO file;
	if (!file.open(m_path.string(), eStreamIOMode::Read))
		return nullptr;

	file.seek(static_cast<size_t>(offset));

	std::unique_ptr<char[]> data(new char[size]);
	file.read(data.get(), static_cast<size_t>(size));
	return data;
}
