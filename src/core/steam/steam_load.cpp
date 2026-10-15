#include <pch.h>
#include <core/steam/steam_load.h>
#include <core/steam/steamcache.h>

static std::string PakStemNoPatch(const std::string& depotPath)
{
	std::filesystem::path p(depotPath);
	std::string stem = p.stem().string();
	// Strip trailing "(NN)" patch suffix if present.
	if (stem.size() > 4 && stem.back() == ')')
	{
		const size_t open = stem.rfind('(');
		if (open != std::string::npos && open + 3 <= stem.size())
			stem = stem.substr(0, open);
	}
	return stem;
}

static bool EndsWithNoCase(const std::string& s, const char* suffix)
{
	const size_t n = strlen(suffix);
	return s.size() >= n && _strnicmp(s.c_str() + s.size() - n, suffix, n) == 0;
}

bool SteamIsSupportedDepotFile(const std::string& depotPath)
{
	return EndsWithNoCase(depotPath, ".rpak") || EndsWithNoCase(depotPath, ".mbnk")
		|| EndsWithNoCase(depotPath, ".mdl") || EndsWithNoCase(depotPath, ".bpk")
		|| EndsWithNoCase(depotPath, "_dir.vpk");
}

bool SteamDownloadRpaksForLoad(
	const std::vector<std::string>& depotSelectedPaths,
	std::vector<std::string>& outLocalPaths,
	std::string& outError,
	std::atomic<uint32_t>* progressCounter)
{
	// Rpaks get patch-chain handling; everything else is downloaded as-is.
	std::vector<std::string> depotRpakPaths;
	std::vector<std::string> otherPaths;
	for (const std::string& path : depotSelectedPaths)
	{
		if (EndsWithNoCase(path, ".rpak"))
			depotRpakPaths.push_back(path);
		else
			otherPaths.push_back(path);
	}

	if (!g_steamClient.HasDepotContext())
	{
		outError = "No Steam depot context is configured";
		return false;
	}

	const auto& ctx = g_steamClient.GetDepotContext();
	const std::filesystem::path root = g_steamCacheManager.GetRPakCacheRoot(ctx.depotId, ctx.manifestId);

	std::vector<SteamDepotFile_t> allFiles;
	if (!g_steamClient.ListManifestFiles(allFiles, outError))
		return false;

	std::unordered_set<std::string> toDownload;
	for (const std::string& path : depotRpakPaths)
		toDownload.insert(CSteamClient::NormalizeDepotPath(path));

	// Try to pull patch_master.rpak if present (only matters when loading rpaks).
	if (!depotRpakPaths.empty())
	{
		for (const auto& file : allFiles)
		{
			if (file.depotPath.ends_with("patch_master.rpak"))
				toDownload.insert(file.depotPath);
		}
	}

	std::vector<std::string> otherNormalized;
	for (const std::string& path : otherPaths)
	{
		const std::string norm = CSteamClient::NormalizeDepotPath(path);
		toDownload.insert(norm);
		otherNormalized.push_back(norm);

		// A *_dir.vpk is useless without its numbered archives (*_000.vpk, ...).
		if (EndsWithNoCase(norm, "_dir.vpk"))
		{
			const std::string prefix = norm.substr(0, norm.size() - strlen("dir.vpk"));
			for (const auto& file : allFiles)
			{
				if (EndsWithNoCase(file.depotPath, ".vpk") && file.depotPath.size() > prefix.size()
					&& _strnicmp(file.depotPath.c_str(), prefix.c_str(), prefix.size()) == 0)
					toDownload.insert(file.depotPath);
			}
		}
	}

	// For each requested pak, pull the full patch chain from the manifest:
	// base + (01) + (02) + ... — LoadAndPatchPakFileData needs every layer,
	// not just the top patch that patch_master points at.
	auto matchesPakStem = [](const std::string& fileStem, const std::string& stem) -> bool
		{
			if (fileStem == stem)
				return true;
			if (fileStem.size() > stem.size() + 3 && fileStem.compare(0, stem.size(), stem) == 0
				&& fileStem[stem.size()] == '(' && fileStem.back() == ')')
				return true;
			return false;
		};

	for (const std::string& requested : depotRpakPaths)
	{
		const std::string stem = PakStemNoPatch(CSteamClient::NormalizeDepotPath(requested));

		for (const auto& file : allFiles)
		{
			std::filesystem::path fp(file.depotPath);
			if (fp.extension() != ".rpak")
				continue;

			if (matchesPakStem(fp.stem().string(), stem))
				toDownload.insert(file.depotPath);
		}
	}

	auto isPlausibleRpak = [](const std::filesystem::path& path) -> bool
		{
			std::error_code ec;
			if (!std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) < 8)
				return false;

			StreamIO file;
			if (!file.open(path.string(), eStreamIOMode::Read))
				return false;

			char magic[4]{};
			file.read(magic, 4);
			return magic[0] == 'R' && magic[1] == 'P' && magic[2] == 'a' && magic[3] == 'k';
		};

	outLocalPaths.clear();
	for (const std::string& depotPath : toDownload)
	{
		const std::filesystem::path dest = root / std::filesystem::path(depotPath);
		std::error_code ec;
		const bool needsDownload = !std::filesystem::exists(dest, ec)
			|| std::filesystem::file_size(dest, ec) == 0
			|| (depotPath.ends_with(".rpak") && !isPlausibleRpak(dest));

		if (needsDownload)
		{
			if (std::filesystem::exists(dest, ec))
				std::filesystem::remove(dest, ec);

			if (!g_steamClient.DownloadFileToPath(depotPath, dest, outError))
				return false;

			if (depotPath.ends_with(".rpak") && !isPlausibleRpak(dest))
			{
				std::filesystem::remove(dest, ec);
				outError = "Downloaded " + depotPath + " is not a valid RPak (missing RPak magic). "
					"Clear Steam cache and re-pin the depot.";
				return false;
			}
		}

		if (progressCounter)
			++(*progressCounter);
	}

	// Hand the loader one path per selected stem. Prefer the base name so
	// HandlePakLoad + patch_master can remap to the top patch; chain siblings
	// stay on disk beside it for LoadAndPatchPakFileData.
	std::unordered_set<std::string> emittedStems;
	for (const std::string& requested : depotRpakPaths)
	{
		const std::string reqNorm = CSteamClient::NormalizeDepotPath(requested);
		const std::string stem = PakStemNoPatch(reqNorm);
		if (!emittedStems.insert(stem).second)
			continue;

		std::filesystem::path preferred;
		std::filesystem::path anyMatch;
		for (const std::string& depotPath : toDownload)
		{
			if (depotPath.ends_with("patch_master.rpak") || !EndsWithNoCase(depotPath, ".rpak"))
				continue;

			const std::string gotStem = PakStemNoPatch(depotPath);
			if (gotStem != stem)
				continue;

			const std::filesystem::path candidate = std::filesystem::absolute(root / std::filesystem::path(depotPath));
			anyMatch = candidate;

			const std::string fileStem = std::filesystem::path(depotPath).stem().string();
			if (fileStem == stem)
			{
				preferred = candidate;
				break;
			}
		}

		const std::filesystem::path& chosen = preferred.empty() ? anyMatch : preferred;
		if (!chosen.empty())
			outLocalPaths.emplace_back(chosen.string());
	}

	for (const std::string& norm : otherNormalized)
		outLocalPaths.emplace_back(std::filesystem::absolute(root / std::filesystem::path(norm)).string());

	if (outLocalPaths.empty())
	{
		outError = "No files were downloaded";
		return false;
	}

	return true;
}
