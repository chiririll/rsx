#pragma once

// Virtual (on-demand) files for the Steam cache.
//
// A path inside the pinned depot's cache root that does not exist on disk is treated as a
// remote depot file: reads are served by fetching byte ranges from Steam instead of
// downloading the whole file. Used for Miles .mstr streaming banks, which can be huge.

// Returns a read-only, seekable streambuf for `path`, or nullptr if it is not a known
// depot file under the current cache root (or exists on disk).
std::unique_ptr<std::streambuf> SteamOpenVirtualFile(const std::filesystem::path& path);

// Appends the cache paths of depot files directly inside `dir` with extension `ext`
// (e.g. ".mstr") that are not present on disk.
void SteamListVirtualFiles(const std::filesystem::path& dir, const char* ext, std::vector<std::filesystem::path>& out);
