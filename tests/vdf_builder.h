#pragma once

#include <core/steam/binary_vdf.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Minimal Steam binary KeyValues writer for unit tests.
class BinaryVdfBuilder
{
public:
	void BeginObject(std::string_view name)
	{
		WriteType(0);
		WriteCString(name);
	}

	void EndObject()
	{
		WriteType(8);
	}

	void AddString(std::string_view name, std::string_view value)
	{
		WriteType(1);
		WriteCString(name);
		WriteCString(value);
	}

	void AddInt32(std::string_view name, int32_t value)
	{
		WriteType(2);
		WriteCString(name);
		WriteBytes(&value, sizeof(value));
	}

	void AddUInt64(std::string_view name, uint64_t value)
	{
		WriteType(7);
		WriteCString(name);
		WriteBytes(&value, sizeof(value));
	}

	const std::vector<uint8_t>& Bytes() const { return m_bytes; }

	bool Parse(BinaryVdfNode& out) const
	{
		return ParseSteamBinaryVdf(m_bytes.data(), m_bytes.size(), out);
	}

private:
	void WriteType(uint8_t type)
	{
		m_bytes.push_back(type);
	}

	void WriteCString(std::string_view s)
	{
		m_bytes.insert(m_bytes.end(), s.begin(), s.end());
		m_bytes.push_back(0);
	}

	void WriteBytes(const void* data, size_t size)
	{
		const auto* p = static_cast<const uint8_t*>(data);
		m_bytes.insert(m_bytes.end(), p, p + size);
	}

	std::vector<uint8_t> m_bytes;
};

inline BinaryVdfBuilder MakeDepotProductInfo(uint32_t depotId, uint64_t publicManifest,
	uint32_t depotFromApp = 0, const char* name = "content", const char* oslist = "windows")
{
	BinaryVdfBuilder b;
	b.BeginObject("appinfo");
	b.BeginObject("depots");

	b.BeginObject(std::to_string(depotId));
	if (name && *name)
		b.AddString("name", name);
	if (depotFromApp != 0)
		b.AddUInt64("depotfromapp", depotFromApp);
	if (oslist && *oslist)
	{
		b.BeginObject("config");
		b.AddString("oslist", oslist);
		b.EndObject();
	}
	if (publicManifest != 0)
	{
		b.BeginObject("manifests");
		b.BeginObject("public");
		b.AddUInt64("gid", publicManifest);
		b.EndObject();
		b.EndObject();
	}
	b.EndObject(); // depot

	b.BeginObject("branches");
	b.AddString("public", "1");
	b.EndObject();

	b.EndObject(); // depots
	b.EndObject(); // appinfo
	return b;
}
