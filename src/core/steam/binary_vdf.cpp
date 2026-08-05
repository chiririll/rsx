#include <core/steam/binary_vdf.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace
{
	enum class KvType : uint8_t
	{
		None = 0,
		String = 1,
		Int32 = 2,
		Float32 = 3,
		Pointer = 4,
		WideString = 5,
		Color = 6,
		UInt64 = 7,
		End = 8,
	};

	bool ReadCString(const uint8_t*& p, const uint8_t* end, std::string& out)
	{
		const uint8_t* start = p;
		while (p < end && *p != 0)
			++p;
		if (p >= end)
			return false;
		out.assign(reinterpret_cast<const char*>(start), static_cast<size_t>(p - start));
		++p; // skip null
		return true;
	}

	template <typename T>
	bool ReadPod(const uint8_t*& p, const uint8_t* end, T& out)
	{
		if (p + sizeof(T) > end)
			return false;
		memcpy(&out, p, sizeof(T));
		p += sizeof(T);
		return true;
	}

	bool ReadNodeChildren(const uint8_t*& p, const uint8_t* end, BinaryVdfNode& parent)
	{
		while (p < end)
		{
			const KvType type = static_cast<KvType>(*p++);
			if (type == KvType::End)
				return true;

			BinaryVdfNode child;
			if (!ReadCString(p, end, child.name))
				return false;

			switch (type)
			{
			case KvType::None:
			{
				if (!ReadNodeChildren(p, end, child))
					return false;
				parent.children.emplace_back(std::move(child));
				break;
			}
			case KvType::String:
			{
				if (!ReadCString(p, end, child.stringValue))
					return false;
				child.hasString = true;
				parent.children.emplace_back(std::move(child));
				break;
			}
			case KvType::Int32:
			{
				int32_t v = 0;
				if (!ReadPod(p, end, v))
					return false;
				child.intValue = static_cast<uint64_t>(static_cast<int64_t>(v));
				child.hasInt = true;
				child.stringValue = std::to_string(v);
				child.hasString = true;
				parent.children.emplace_back(std::move(child));
				break;
			}
			case KvType::UInt64:
			{
				uint64_t v = 0;
				if (!ReadPod(p, end, v))
					return false;
				child.intValue = v;
				child.hasInt = true;
				child.stringValue = std::to_string(v);
				child.hasString = true;
				parent.children.emplace_back(std::move(child));
				break;
			}
			case KvType::Float32:
			{
				float v = 0.f;
				if (!ReadPod(p, end, v))
					return false;
				child.stringValue = std::to_string(v);
				child.hasString = true;
				parent.children.emplace_back(std::move(child));
				break;
			}
			case KvType::Color:
			case KvType::Pointer:
			{
				uint32_t v = 0;
				if (!ReadPod(p, end, v))
					return false;
				child.intValue = v;
				child.hasInt = true;
				parent.children.emplace_back(std::move(child));
				break;
			}
			case KvType::WideString:
			{
				// Length-prefixed UTF-16 (uint16 char count including null) in some variants;
				// Steam appinfo often uses a 16-bit length then wchar bytes.
				uint16_t len = 0;
				if (!ReadPod(p, end, len))
					return false;
				const size_t bytes = static_cast<size_t>(len) * sizeof(wchar_t);
				if (p + bytes > end)
					return false;
				p += bytes;
				parent.children.emplace_back(std::move(child));
				break;
			}
			default:
				return false;
			}
		}
		return true;
	}
}

const BinaryVdfNode* BinaryVdfNode::FindChild(std::string_view childName) const
{
	for (const BinaryVdfNode& child : children)
	{
		if (child.name == childName)
			return &child;
	}
	return nullptr;
}

const BinaryVdfNode* BinaryVdfNode::FindChildRecursive(std::string_view childName) const
{
	if (const BinaryVdfNode* direct = FindChild(childName))
		return direct;
	for (const BinaryVdfNode& child : children)
	{
		if (const BinaryVdfNode* found = child.FindChildRecursive(childName))
			return found;
	}
	return nullptr;
}

std::string BinaryVdfNode::GetString(std::string_view childName) const
{
	const BinaryVdfNode* child = FindChild(childName);
	if (!child)
		return {};
	if (child->hasString)
		return child->stringValue;
	if (child->hasInt)
		return std::to_string(child->intValue);
	return {};
}

uint64_t BinaryVdfNode::GetUInt64(std::string_view childName) const
{
	const BinaryVdfNode* child = FindChild(childName);
	if (!child)
		return 0;
	if (child->hasInt)
		return child->intValue;
	if (child->hasString)
		return std::strtoull(child->stringValue.c_str(), nullptr, 10);
	return 0;
}

bool ParseSteamBinaryVdf(const void* data, size_t size, BinaryVdfNode& outRoot)
{
	outRoot = {};
	if (!data || size == 0)
		return false;

	const uint8_t* p = static_cast<const uint8_t*>(data);
	const uint8_t* end = p + size;
	outRoot.name = "root";
	return ReadNodeChildren(p, end, outRoot);
}

bool ParseSteamProductInfo(const void* data, size_t size, BinaryVdfNode& outRoot)
{
	if (!data || size == 0)
		return false;

	const auto* bytes = static_cast<const uint8_t*>(data);

	// Text VDF usually starts with quote or whitespace + quote / identifier.
	const bool looksText = bytes[0] == '"' || bytes[0] == '/' || bytes[0] == 'a' || bytes[0] == 'A'
		|| (bytes[0] == '\n' || bytes[0] == '\r' || bytes[0] == ' ' || bytes[0] == '\t');

	if (looksText && bytes[0] == '"')
	{
		// Caller can use text parser; treat as failure here so it can fall back.
		return false;
	}

	// Try raw binary KV.
	if (ParseSteamBinaryVdf(data, size, outRoot) && !outRoot.children.empty())
		return true;

	// Some PICS payloads are prefixed with a 40-byte hash or similar header.
	for (const size_t skip : { 40u, 20u, 4u, 8u })
	{
		if (size <= skip)
			continue;
		if (ParseSteamBinaryVdf(bytes + skip, size - skip, outRoot) && !outRoot.children.empty())
			return true;
	}

	return false;
}
