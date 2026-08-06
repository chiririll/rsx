#pragma once

#include <core/steam/binary_vdf.h>
#include <core/steam/steam_types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ResolvedDepot_t
{
	uint32_t keyAppId = 0;
	uint64_t manifestId = 0;
};

using ProductInfoProvider = std::function<bool(uint32_t appId, BinaryVdfNode& outRoot, std::string& outError)>;

uint64_t ManifestIdFromDepotNode(const BinaryVdfNode& depotNode, const std::string& branch);
std::string NormalizeDepotPath(const std::string& path);
int PickPreferredDepotIndex(const std::vector<SteamDepotInfo_t>& depots);

bool ResolveDepotTarget(uint32_t appId, uint32_t depotId, const std::string& branch,
	uint64_t requestedManifestId, const ProductInfoProvider& provider,
	ResolvedDepot_t& out, std::string& outError);

bool BuildDepotList(uint32_t appId, const std::string& branch, const ProductInfoProvider& provider,
	std::vector<SteamDepotInfo_t>& outDepots, std::string& outError);

bool BuildBranchList(uint32_t appId, const ProductInfoProvider& provider,
	std::vector<SteamBranchInfo_t>& outBranches, std::string& outError);
