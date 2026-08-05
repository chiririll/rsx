# tek-steamclient (vendored)

Windows x64 bits of [tek-steamclient](https://github.com/teknology-hub/tek-steamclient) v2.1.5, used by RSX for Steam CDN depot chunk downloads.

| Path | Source |
|------|--------|
| `include/tek-steamclient/*.h` | Upstream headers (with small MSVC compatibility patches) |
| `libtek-steamclient-2.lib` / `.def` | MSVC import library generated from the release DLL |
| `libtek-steamclient-2.dll` | Fetched by `tools/prebuild.ps1` (not committed) |
| `COPYING` | GPL-3.0-or-later |

Post-build copies the DLL next to `rsx.exe`.
