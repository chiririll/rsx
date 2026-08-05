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

bool SteamDownloadRpaksForLoad(
	const std::vector<std::string>& depotRpakPaths,
	std::vector<std::string>& outLocalPaths,
	std::string& outError,
	std::atomic<uint32_t>* progressCounter)
{
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

	// Always try to pull patch_master.rpak if present.
	for (const auto& file : allFiles)
	{
		if (file.depotPath.ends_with("patch_master.rpak"))
			toDownload.insert(file.depotPath);
	}

	// For each requested base pak, also pull the highest patch variant in the manifest.
	for (const std::string& requested : depotRpakPaths)
	{
		const std::string norm = CSteamClient::NormalizeDepotPath(requested);
		const std::string stem = PakStemNoPatch(norm);
		std::string best;
		int bestPatch = -1;

		for (const auto& file : allFiles)
		{
			std::filesystem::path fp(file.depotPath);
			if (fp.extension() != ".rpak")
				continue;

			const std::string fileStem = fp.stem().string();
			if (fileStem == stem)
			{
				if (bestPatch < 0)
				{
					best = file.depotPath;
					bestPatch = 0;
				}
				continue;
			}

			// Match stem(NN)
			if (fileStem.size() > stem.size() + 3 && fileStem.compare(0, stem.size(), stem) == 0
				&& fileStem[stem.size()] == '(' && fileStem.back() == ')')
			{
				const std::string num = fileStem.substr(stem.size() + 1, fileStem.size() - stem.size() - 2);
				const int patch = atoi(num.c_str());
				if (patch > bestPatch)
				{
					bestPatch = patch;
					best = file.depotPath;
				}
			}
		}

		if (!best.empty())
			toDownload.insert(best);
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

		// Only return user-selected (or their resolved patch) paths to the loader,
		// not patch_master alone.
		bool isSelected = false;
		for (const std::string& requested : depotRpakPaths)
		{
			const std::string reqNorm = CSteamClient::NormalizeDepotPath(requested);
			const std::string reqStem = PakStemNoPatch(reqNorm);
			const std::string gotStem = PakStemNoPatch(depotPath);
			if (reqStem == gotStem)
			{
				isSelected = true;
				break;
			}
		}

		if (isSelected)
			outLocalPaths.emplace_back(std::filesystem::absolute(dest).string());

		if (progressCounter)
			++(*progressCounter);
	}

	if (outLocalPaths.empty())
	{
		outError = "No rpak files were downloaded";
		return false;
	}

	return true;
}
