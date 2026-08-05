#pragma once

// Minimal Steam binary KeyValues reader (SteamKit / Source style).
// Type 0 + name = nested object, type 8 = end of children.
struct BinaryVdfNode
{
	std::string name;
	std::string stringValue;
	uint64_t intValue = 0;
	bool hasString = false;
	bool hasInt = false;
	std::vector<BinaryVdfNode> children;

	const BinaryVdfNode* FindChild(std::string_view childName) const;
	const BinaryVdfNode* FindChildRecursive(std::string_view childName) const;
	std::string GetString(std::string_view childName) const;
	uint64_t GetUInt64(std::string_view childName) const;
};

bool ParseSteamBinaryVdf(const void* data, size_t size, BinaryVdfNode& outRoot);
bool ParseSteamProductInfo(const void* data, size_t size, BinaryVdfNode& outRoot);
