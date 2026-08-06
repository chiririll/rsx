#pragma once

#include <cstdint>
#include <string>

struct SteamDepotFile_t
{
	std::string depotPath; // depot-relative path using '/' separators
	int64_t size = 0;
};

struct SteamDepotContext_t
{
	uint32_t appId = 0;
	uint32_t depotId = 0;
	// App ID used for GetDepotDecryptionKey (may differ when depotfromapp is set).
	uint32_t keyAppId = 0;
	uint64_t manifestId = 0;
	std::string branch = "public";
};

struct SteamChunkRef_t
{
	std::string sha1Hex;
	int64_t offset = 0;
	int size = 0;
	int compSize = 0;
};

struct SteamDepotInfo_t
{
	uint32_t depotId = 0;
	std::string name;
	uint64_t branchManifestId = 0; // manifest for the requested branch (usually public)
	uint32_t depotFromApp = 0; // shared depot parent app, if any
	std::string oslist;
};

struct SteamBranchInfo_t
{
	std::string name;
	std::string description;
	uint64_t buildId = 0;
	bool passwordRequired = false;
};
