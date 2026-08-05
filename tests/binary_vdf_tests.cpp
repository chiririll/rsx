#include <doctest/doctest.h>

#include "vdf_builder.h"

#include <core/steam/binary_vdf.h>

#include <cstring>
#include <vector>

TEST_SUITE("binary_vdf")
{
	TEST_CASE("ParseSteamBinaryVdf reads nested string int32 and uint64")
	{
		BinaryVdfBuilder b;
		b.BeginObject("depots");
		b.BeginObject("1172471");
		b.AddString("name", "content");
		b.AddInt32("size", 42);
		b.BeginObject("manifests");
		b.BeginObject("public");
		b.AddUInt64("gid", 6021239266545804912ull);
		b.EndObject();
		b.EndObject();
		b.EndObject();
		b.EndObject();

		BinaryVdfNode root;
		REQUIRE(b.Parse(root));

		const BinaryVdfNode* depots = root.FindChild("depots");
		REQUIRE(depots != nullptr);
		const BinaryVdfNode* depot = depots->FindChild("1172471");
		REQUIRE(depot != nullptr);
		CHECK(depot->GetString("name") == "content");
		CHECK(depot->GetUInt64("size") == 42);

		const BinaryVdfNode* gid = depot->FindChildRecursive("gid");
		REQUIRE(gid != nullptr);
		CHECK(gid->hasInt);
		CHECK(gid->intValue == 6021239266545804912ull);
	}

	TEST_CASE("ParseSteamBinaryVdf rejects truncated and unknown type buffers")
	{
		BinaryVdfNode root;
		CHECK_FALSE(ParseSteamBinaryVdf(nullptr, 0, root));

		const uint8_t truncated[] = { 1, 'n', 'a', 'm', 'e', 0 }; // string type without value
		CHECK_FALSE(ParseSteamBinaryVdf(truncated, sizeof(truncated), root));

		const uint8_t unknownType[] = { 99, 'x', 0 };
		CHECK_FALSE(ParseSteamBinaryVdf(unknownType, sizeof(unknownType), root));
	}

	TEST_CASE("ParseSteamProductInfo accepts raw binary and skip-prefixed payloads")
	{
		BinaryVdfBuilder b;
		b.BeginObject("appinfo");
		b.AddString("name", "Titanfall 2");
		b.EndObject();

		BinaryVdfNode root;
		REQUIRE(ParseSteamProductInfo(b.Bytes().data(), b.Bytes().size(), root));
		CHECK(root.FindChild("appinfo") != nullptr);
		CHECK(root.GetString("name").empty()); // name is nested under appinfo
		CHECK(root.FindChild("appinfo")->GetString("name") == "Titanfall 2");

		std::vector<uint8_t> prefixed(40, 0xAB);
		prefixed.insert(prefixed.end(), b.Bytes().begin(), b.Bytes().end());
		BinaryVdfNode prefixedRoot;
		REQUIRE(ParseSteamProductInfo(prefixed.data(), prefixed.size(), prefixedRoot));
		CHECK(prefixedRoot.FindChild("appinfo")->GetString("name") == "Titanfall 2");
	}

	TEST_CASE("ParseSteamProductInfo rejects text VDF starting with quote")
	{
		const char* text = "\"appinfo\"\n{\n\"name\" \"x\"\n}\n";
		BinaryVdfNode root;
		CHECK_FALSE(ParseSteamProductInfo(text, std::strlen(text), root));
	}

	TEST_CASE("BinaryVdfNode FindChildRecursive and GetUInt64 from string")
	{
		BinaryVdfBuilder b;
		b.BeginObject("outer");
		b.BeginObject("inner");
		b.AddString("id", "12345");
		b.EndObject();
		b.EndObject();

		BinaryVdfNode root;
		REQUIRE(b.Parse(root));
		const BinaryVdfNode* id = root.FindChildRecursive("id");
		REQUIRE(id != nullptr);
		CHECK(id->hasString);
		CHECK(root.FindChild("outer")->GetUInt64("missing") == 0);
		CHECK(root.FindChild("outer")->FindChild("inner")->GetUInt64("id") == 12345ull);
	}
}
