#pragma once

#include <core/steam/steamclient.h>

// Download selected rpaks into the Steam rpak cache (depot layout), including
// patch_master and the full patch chain for each stem (base + (01) + …).
// Returns one local path per selected stem for HandlePakLoad.
bool SteamDownloadRpaksForLoad(
	const std::vector<std::string>& depotRpakPaths,
	std::vector<std::string>& outLocalPaths,
	std::string& outError,
	std::atomic<uint32_t>* progressCounter = nullptr);
