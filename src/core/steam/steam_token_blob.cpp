#include <core/steam/steam_token_blob.h>

#include <cstring>

std::vector<char> SerializeAuthToken(const std::string& username, const std::string& token)
{
	std::vector<char> blob;
	const auto appendStr = [&](const std::string& s)
		{
			const uint32_t len = static_cast<uint32_t>(s.size());
			const size_t off = blob.size();
			blob.resize(off + 4 + s.size());
			memcpy(blob.data() + off, &len, 4);
			memcpy(blob.data() + off + 4, s.data(), s.size());
		};
	appendStr(username);
	appendStr(token);
	return blob;
}

bool DeserializeAuthToken(const void* data, size_t size, std::string& outUsername, std::string& outToken)
{
	if (!data || size < 8)
		return false;

	const auto* bytes = static_cast<const char*>(data);
	size_t pos = 0;
	const auto readStr = [&](std::string& out) -> bool
		{
			if (pos + 4 > size)
				return false;
			uint32_t len = 0;
			memcpy(&len, bytes + pos, 4);
			pos += 4;
			if (pos + len > size)
				return false;
			out.assign(bytes + pos, len);
			pos += len;
			return true;
		};

	return readStr(outUsername) && readStr(outToken);
}
