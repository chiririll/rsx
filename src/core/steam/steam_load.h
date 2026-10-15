#pragma once

#include <core/steam/steamclient.h>

// True for depot files the loader can open: .rpak, .mbnk, .mdl, .bpk, *_dir.vpk.
bool SteamIsSupportedDepotFile(const std::string& depotPath);

// Download selected files into the Steam rpak cache (depot layout). For rpaks this
// includes patch_master and the full patch chain for each stem (base + (01) + …);
// for *_dir.vpk the numbered archives beside it are pulled too.
// Returns one local path per selected stem/file for HandleFileLoad.
bool SteamDownloadRpaksForLoad(
	const std::vector<std::string>& depotRpakPaths,
	std::vector<std::string>& outLocalPaths,
	std::string& outError,
	std::atomic<uint32_t>* progressCounter = nullptr,
	const std::function<void(uint32_t)>& onTotalChunks = nullptr);
// progressCounter counts downloaded chunks; onTotalChunks is called once, before the first
// download, with the number of chunks that will be fetched (0 if everything is cached).
