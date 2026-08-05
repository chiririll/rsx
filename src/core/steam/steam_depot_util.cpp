#include <core/steam/steam_depot_util.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

uint64_t ManifestIdFromDepotNode(const BinaryVdfNode& depotNode, const std::string& branch)
{
	const BinaryVdfNode* manifests = depotNode.FindChild("manifests");
	if (!manifests)
		return 0;

	auto readBranch = [&](std::string_view name) -> uint64_t
		{
			const BinaryVdfNode* branchNode = manifests->FindChild(name);
			if (!branchNode)
				return 0;
			if (branchNode->hasString)
				return std::strtoull(branchNode->stringValue.c_str(), nullptr, 10);
			if (branchNode->hasInt)
				return branchNode->intValue;
			const uint64_t gid = branchNode->GetUInt64("gid");
			if (gid != 0)
				return gid;
			return branchNode->GetUInt64("id");
		};

	uint64_t id = readBranch(branch);
	if (id == 0 && branch != "public")
		id = readBranch("public");
	return id;
}

std::string NormalizeDepotPath(const std::string& path)
{
	std::string out = path;
	std::replace(out.begin(), out.end(), '\\', '/');
	while (!out.empty() && (out.front() == '/' || out.front() == '.'))
	{
		if (out.front() == '.')
		{
			if (out.size() >= 2 && out[1] == '/')
				out.erase(0, 2);
			else
				break;
		}
		else
			out.erase(out.begin());
	}
	std::transform(out.begin(), out.end(), out.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return out;
}

int PickPreferredDepotIndex(const std::vector<SteamDepotInfo_t>& depots)
{
	auto isWindows = [](const SteamDepotInfo_t& d) -> bool
		{
			if (d.oslist.empty())
				return true;
			std::string lower = d.oslist;
			std::transform(lower.begin(), lower.end(), lower.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return lower.find("windows") != std::string::npos || lower.find("win32") != std::string::npos;
		};

	int best = -1;
	int bestScore = -1;
	for (int i = 0; i < static_cast<int>(depots.size()); ++i)
	{
		const SteamDepotInfo_t& d = depots[static_cast<size_t>(i)];
		int score = 0;
		if (d.branchManifestId != 0)
			score += 10;
		if (isWindows(d))
			score += 5;
		if (d.depotFromApp == 0)
			score += 2;

		std::string name = d.name;
		std::transform(name.begin(), name.end(), name.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (name.find("content") != std::string::npos)
			score += 3;
		if (name.find("audio") != std::string::npos || name.find("video") != std::string::npos
			|| name.find("soundtrack") != std::string::npos)
			score -= 4;

		if (score > bestScore)
		{
			bestScore = score;
			best = i;
		}
	}
	return best;
}

bool ResolveDepotTarget(uint32_t appId, uint32_t depotId, const std::string& branch,
	uint64_t requestedManifestId, const ProductInfoProvider& provider,
	ResolvedDepot_t& out, std::string& outError)
{
	out = {};
	out.keyAppId = appId;
	out.manifestId = requestedManifestId;

	const std::string branchName = branch.empty() ? "public" : branch;

	BinaryVdfNode root;
	std::string productError;
	const bool haveProductInfo = provider && provider(appId, root, productError);
	if (!haveProductInfo && out.manifestId == 0)
	{
		outError = productError.empty() ? "Failed to get Steam product info for app" : productError;
		return false;
	}

	if (haveProductInfo)
	{
		const BinaryVdfNode* depots = root.FindChildRecursive("depots");
		const BinaryVdfNode* depotNode = depots ? depots->FindChild(std::to_string(depotId)) : nullptr;
		if (depotNode)
		{
			const uint32_t depotFromApp = static_cast<uint32_t>(depotNode->GetUInt64("depotfromapp"));
			if (depotFromApp != 0)
				out.keyAppId = depotFromApp;

			if (out.manifestId == 0)
			{
				out.manifestId = ManifestIdFromDepotNode(*depotNode, branchName);
				if (out.manifestId == 0 && depotFromApp != 0 && depotFromApp != appId && provider)
				{
					BinaryVdfNode parentRoot;
					std::string parentError;
					if (provider(depotFromApp, parentRoot, parentError))
					{
						const BinaryVdfNode* parentDepots = parentRoot.FindChildRecursive("depots");
						const BinaryVdfNode* parentDepot = parentDepots
							? parentDepots->FindChild(std::to_string(depotId))
							: nullptr;
						if (parentDepot)
							out.manifestId = ManifestIdFromDepotNode(*parentDepot, branchName);
					}
				}
			}
		}
		else if (out.manifestId == 0)
		{
			outError = "Depot " + std::to_string(depotId) + " not found in app product info";
			return false;
		}
	}

	if (out.manifestId == 0)
	{
		outError = "Could not resolve manifest ID for depot/branch; enter a manifest ID manually";
		return false;
	}

	return true;
}

bool BuildDepotList(uint32_t appId, const std::string& branch, const ProductInfoProvider& provider,
	std::vector<SteamDepotInfo_t>& outDepots, std::string& outError)
{
	if (!provider)
	{
		outError = "No product info provider";
		return false;
	}

	BinaryVdfNode root;
	if (!provider(appId, root, outError))
		return false;

	const BinaryVdfNode* depots = root.FindChildRecursive("depots");
	if (!depots)
	{
		outError = "Product info has no depots section";
		return false;
	}

	outDepots.clear();
	const std::string branchName = branch.empty() ? "public" : branch;
	for (const BinaryVdfNode& child : depots->children)
	{
		// Depot keys are numeric; skip "branches", "baselanguages", etc.
		if (child.name.empty() || !std::isdigit(static_cast<unsigned char>(child.name[0])))
			continue;

		SteamDepotInfo_t info;
		info.depotId = static_cast<uint32_t>(std::strtoul(child.name.c_str(), nullptr, 10));
		if (info.depotId == 0)
			continue;

		info.name = child.GetString("name");
		info.depotFromApp = static_cast<uint32_t>(child.GetUInt64("depotfromapp"));
		if (const BinaryVdfNode* config = child.FindChild("config"))
			info.oslist = config->GetString("oslist");
		info.branchManifestId = ManifestIdFromDepotNode(child, branchName);

		// Shared depots may only list manifests on the parent app.
		if (info.branchManifestId == 0 && info.depotFromApp != 0 && info.depotFromApp != appId)
		{
			BinaryVdfNode parentRoot;
			std::string parentError;
			if (provider(info.depotFromApp, parentRoot, parentError))
			{
				const BinaryVdfNode* parentDepots = parentRoot.FindChildRecursive("depots");
				const BinaryVdfNode* parentDepot = parentDepots
					? parentDepots->FindChild(std::to_string(info.depotId))
					: nullptr;
				if (parentDepot)
					info.branchManifestId = ManifestIdFromDepotNode(*parentDepot, branchName);
			}
		}

		outDepots.emplace_back(std::move(info));
	}

	std::sort(outDepots.begin(), outDepots.end(), [](const SteamDepotInfo_t& a, const SteamDepotInfo_t& b)
		{
			return a.depotId < b.depotId;
		});

	if (outDepots.empty())
	{
		outError = "No depots found in product info";
		return false;
	}
	return true;
}
