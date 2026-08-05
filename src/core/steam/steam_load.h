#pragma once

#include <core/steam/steamclient.h>

// Download selected rpaks (plus patch_master / patch variants when present in
// the manifest) into the local Steam rpak cache directory mirroring depot layout,
// then return absolute paths suitable for HandlePakLoad.
bool SteamDownloadRpaksForLoad(
	const std::vector<std::string>& depotRpakPaths,
	std::vector<std::string>& outLocalPaths,
	std::string& outError,
	std::atomic<uint32_t>* progressCounter = nullptr);
