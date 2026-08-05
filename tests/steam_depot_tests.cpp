#include <doctest/doctest.h>

#include "vdf_builder.h"

#include <core/steam/steam_depot_util.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	ProductInfoProvider MakeMapProvider(std::unordered_map<uint32_t, BinaryVdfBuilder> trees)
	{
		return [trees = std::move(trees)](uint32_t appId, BinaryVdfNode& out, std::string& err) -> bool
			{
				const auto it = trees.find(appId);
				if (it == trees.end())
				{
					err = "no product info for " + std::to_string(appId);
					return false;
				}
				if (!it->second.Parse(out))
				{
					err = "parse failed";
					return false;
				}
				return true;
			};
	}
}

TEST_SUITE("steam_depot_util")
{
	TEST_CASE("ManifestIdFromDepotNode reads string nested gid and falls back to public")
	{
		BinaryVdfBuilder stringBranch;
		stringBranch.BeginObject("depot");
		stringBranch.BeginObject("manifests");
		stringBranch.AddString("public", "111");
		stringBranch.EndObject();
		stringBranch.EndObject();
		BinaryVdfNode stringRoot;
		REQUIRE(stringBranch.Parse(stringRoot));
		CHECK(ManifestIdFromDepotNode(*stringRoot.FindChild("depot"), "public") == 111ull);

		BinaryVdfBuilder gidBranch;
		gidBranch.BeginObject("depot");
		gidBranch.BeginObject("manifests");
		gidBranch.BeginObject("public");
		gidBranch.AddUInt64("gid", 222ull);
		gidBranch.EndObject();
		gidBranch.EndObject();
		gidBranch.EndObject();
		BinaryVdfNode gidRoot;
		REQUIRE(gidBranch.Parse(gidRoot));
		CHECK(ManifestIdFromDepotNode(*gidRoot.FindChild("depot"), "beta") == 222ull);

		BinaryVdfBuilder empty;
		empty.BeginObject("depot");
		empty.AddString("name", "x");
		empty.EndObject();
		BinaryVdfNode emptyRoot;
		REQUIRE(empty.Parse(emptyRoot));
		CHECK(ManifestIdFromDepotNode(*emptyRoot.FindChild("depot"), "public") == 0ull);
	}

	TEST_CASE("NormalizeDepotPath trims separators dots and lowercases")
	{
		CHECK(NormalizeDepotPath("R2\\paks\\Win64\\foo.rpak") == "r2/paks/win64/foo.rpak");
		CHECK(NormalizeDepotPath("/./Foo/Bar") == "foo/bar");
		CHECK(NormalizeDepotPath("///a") == "a");
	}

	TEST_CASE("PickPreferredDepotIndex prefers windows content with manifest")
	{
		std::vector<SteamDepotInfo_t> depots(3);
		depots[0].depotId = 1;
		depots[0].name = "audio";
		depots[0].oslist = "windows";
		depots[0].branchManifestId = 10;

		depots[1].depotId = 2;
		depots[1].name = "game content";
		depots[1].oslist = "windows";
		depots[1].branchManifestId = 20;

		depots[2].depotId = 3;
		depots[2].name = "content";
		depots[2].oslist = "linux";
		depots[2].branchManifestId = 0;

		CHECK(PickPreferredDepotIndex(depots) == 1);
		CHECK(PickPreferredDepotIndex({}) == -1);
	}

	TEST_CASE("ResolveDepotTarget uses explicit manifest and depotfromapp keyAppId")
	{
		auto provider = MakeMapProvider({
			{ 1237970u, MakeDepotProductInfo(1172471u, 999ull, 555u) },
			});

		ResolvedDepot_t resolved;
		std::string err;
		REQUIRE(ResolveDepotTarget(1237970u, 1172471u, "public", 42ull, provider, resolved, err));
		CHECK(resolved.manifestId == 42ull);
		CHECK(resolved.keyAppId == 555u);
	}

	TEST_CASE("ResolveDepotTarget pulls manifest from parent app via depotfromapp")
	{
		BinaryVdfBuilder childApp = MakeDepotProductInfo(1172471u, 0, 99u, "shared", "windows");
		BinaryVdfBuilder parentApp = MakeDepotProductInfo(1172471u, 777ull, 0, "shared", "windows");

		auto provider = MakeMapProvider({
			{ 10u, std::move(childApp) },
			{ 99u, std::move(parentApp) },
			});

		ResolvedDepot_t resolved;
		std::string err;
		REQUIRE(ResolveDepotTarget(10u, 1172471u, "public", 0, provider, resolved, err));
		CHECK(resolved.keyAppId == 99u);
		CHECK(resolved.manifestId == 777ull);
	}

	TEST_CASE("ResolveDepotTarget errors for unknown depot without explicit manifest")
	{
		auto provider = MakeMapProvider({
			{ 1u, MakeDepotProductInfo(2u, 5ull) },
			});

		ResolvedDepot_t resolved;
		std::string err;
		CHECK_FALSE(ResolveDepotTarget(1u, 999u, "public", 0, provider, resolved, err));
		CHECK(err.find("not found") != std::string::npos);
	}

	TEST_CASE("ResolveDepotTarget succeeds with explicit manifest when provider fails")
	{
		auto provider = [](uint32_t, BinaryVdfNode&, std::string& err) -> bool
			{
				err = "offline";
				return false;
			};

		ResolvedDepot_t resolved;
		std::string err;
		REQUIRE(ResolveDepotTarget(1u, 2u, "public", 123ull, provider, resolved, err));
		CHECK(resolved.manifestId == 123ull);
		CHECK(resolved.keyAppId == 1u);
	}

	TEST_CASE("BuildDepotList skips branches sorts and reads oslist")
	{
		BinaryVdfBuilder multi;
		multi.BeginObject("appinfo");
		multi.BeginObject("depots");
		multi.BeginObject("30");
		multi.AddString("name", "second");
		multi.BeginObject("config");
		multi.AddString("oslist", "windows");
		multi.EndObject();
		multi.BeginObject("manifests");
		multi.BeginObject("public");
		multi.AddUInt64("gid", 30ull);
		multi.EndObject();
		multi.EndObject();
		multi.EndObject();
		multi.BeginObject("10");
		multi.AddString("name", "first");
		multi.BeginObject("manifests");
		multi.BeginObject("public");
		multi.AddUInt64("gid", 10ull);
		multi.EndObject();
		multi.EndObject();
		multi.EndObject();
		multi.BeginObject("branches");
		multi.AddString("public", "1");
		multi.EndObject();
		multi.EndObject();
		multi.EndObject();

		auto provider = MakeMapProvider({ { 1u, std::move(multi) } });

		std::vector<SteamDepotInfo_t> depots;
		std::string err;
		REQUIRE(BuildDepotList(1u, "public", provider, depots, err));
		REQUIRE(depots.size() == 2);
		CHECK(depots[0].depotId == 10u);
		CHECK(depots[1].depotId == 30u);
		CHECK(depots[1].oslist == "windows");
		CHECK(depots[1].branchManifestId == 30ull);
	}

	TEST_CASE("BuildDepotList errors when no numeric depots present")
	{
		BinaryVdfBuilder onlyBranches;
		onlyBranches.BeginObject("appinfo");
		onlyBranches.BeginObject("depots");
		onlyBranches.BeginObject("branches");
		onlyBranches.AddString("public", "1");
		onlyBranches.EndObject();
		onlyBranches.EndObject();
		onlyBranches.EndObject();

		auto provider = MakeMapProvider({ { 1u, std::move(onlyBranches) } });
		std::vector<SteamDepotInfo_t> depots;
		std::string err;
		CHECK_FALSE(BuildDepotList(1u, "public", provider, depots, err));
		CHECK(err.find("No depots") != std::string::npos);
	}
}
