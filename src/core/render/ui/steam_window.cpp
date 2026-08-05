#include <pch.h>
#include <core/render/ui/steam_window.h>

#ifndef RTECH_STATIC_LIB

#include <condition_variable>

#include <thirdparty/imgui/misc/imgui_utility.h>
#include <thirdparty/qrcodegen/qrcodegen.hpp>
#include <misc/ImGuiNotify.hpp>

#include <core/steam/steamclient.h>
#include <core/steam/steamcache.h>
#include <core/steam/steam_load.h>
#include <core/filehandling/load.h>

struct SteamWindowState_t
{
	bool open = false;

	char username[128]{};
	char password[128]{};
	char guardCode[32]{};
	bool rememberLogin = true;
	bool awaitingGuard = false;
	std::string guardPrompt;
	std::mutex guardMutex;
	std::condition_variable guardCv;
	bool guardReady = false;
	bool guardCancelled = false;

	// QR login
	bool qrActive = false;
	bool qrAwaitingConfirm = false;
	std::atomic<bool> qrCancel{ false };
	std::mutex qrMutex;
	std::string qrUrl;
	std::string qrEncodedUrl;
	std::unique_ptr<qrcodegen::QrCode> qrCode;

	char appId[32]{ "1237970" }; // Titanfall 2 default; user can change
	char depotId[32]{};
	char branch[64]{ "public" };
	char manifestId[64]{}; // empty = latest for branch

	std::vector<SteamDepotFile_t> rpakFiles;
	std::vector<bool> selected;
	char filter[128]{};

	std::string status;
	bool busy = false;
};

static SteamWindowState_t s_steamUi;

void OpenSteamLoadWindow()
{
	s_steamUi.open = true;
	if (s_steamUi.username[0] == '\0')
	{
		const std::string remembered = g_steamClient.GetRememberedUsername();
		if (!remembered.empty())
			strncpy_s(s_steamUi.username, remembered.c_str(), _TRUNCATE);
	}
}

static void SteamStatus(const std::string& msg)
{
	s_steamUi.status = msg;
	Log("STEAM: %s\n", msg.c_str());
}

static void RefreshQrCodeIfNeeded()
{
	std::string url;
	{
		std::lock_guard lock(s_steamUi.qrMutex);
		url = s_steamUi.qrUrl;
	}

	if (url.empty() || url == s_steamUi.qrEncodedUrl)
		return;

	try
	{
		s_steamUi.qrCode = std::make_unique<qrcodegen::QrCode>(
			qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM));
		s_steamUi.qrEncodedUrl = url;
	}
	catch (const std::exception& ex)
	{
		SteamStatus(std::string("Failed to encode QR: ") + ex.what());
	}
}

static void DrawQrCodeWidget()
{
	RefreshQrCodeIfNeeded();

	if (!s_steamUi.qrCode)
		return;

	const qrcodegen::QrCode& qr = *s_steamUi.qrCode;
	const int modules = qr.getSize();
	if (modules <= 0)
		return;

	constexpr float kDisplaySize = 220.f;
	constexpr int kBorder = 2;
	const float cell = kDisplaySize / static_cast<float>(modules + kBorder * 2);

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList* const draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(origin,
		ImVec2(origin.x + kDisplaySize, origin.y + kDisplaySize),
		IM_COL32(255, 255, 255, 255));

	for (int y = 0; y < modules; ++y)
	{
		for (int x = 0; x < modules; ++x)
		{
			if (!qr.getModule(x, y))
				continue;

			const float x0 = origin.x + (x + kBorder) * cell;
			const float y0 = origin.y + (y + kBorder) * cell;
			draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + cell + 0.5f, y0 + cell + 0.5f),
				IM_COL32(0, 0, 0, 255));
		}
	}

	ImGui::Dummy(ImVec2(kDisplaySize, kDisplaySize));
}

static void StartQrLogin()
{
	s_steamUi.busy = true;
	s_steamUi.qrActive = true;
	s_steamUi.qrAwaitingConfirm = false;
	s_steamUi.qrCancel.store(false);
	{
		std::lock_guard lock(s_steamUi.qrMutex);
		s_steamUi.qrUrl.clear();
		s_steamUi.qrEncodedUrl.clear();
	}
	SteamStatus("Waiting for Steam QR...");

	CThread([]()
		{
			std::string error;
			const bool ok = g_steamClient.LoginWithQr(s_steamUi.rememberLogin,
				[](const std::string& url)
				{
					std::lock_guard lock(s_steamUi.qrMutex);
					if (url.empty())
					{
						s_steamUi.qrAwaitingConfirm = true;
						s_steamUi.status = "Confirm login in the Steam mobile app...";
					}
					else
					{
						s_steamUi.qrUrl = url;
						s_steamUi.qrAwaitingConfirm = false;
						s_steamUi.status = "Scan the QR code with the Steam mobile app";
					}
				},
				&s_steamUi.qrCancel, error);

			s_steamUi.qrActive = false;
			s_steamUi.qrAwaitingConfirm = false;
			{
				std::lock_guard lock(s_steamUi.qrMutex);
				s_steamUi.qrUrl.clear();
			}

			if (!ok)
				SteamStatus("QR login failed: " + error);
			else
			{
				SteamStatus("Logged in via QR");
				const std::string user = g_steamClient.GetRememberedUsername();
				if (!user.empty())
					strncpy_s(s_steamUi.username, user.c_str(), _TRUNCATE);
			}
			s_steamUi.busy = false;
		}).detach();
}

static void DrawSteamLoadWindow()
{
	if (!s_steamUi.open)
		return;

	ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Load from Steam", &s_steamUi.open))
	{
		ImGui::End();
		return;
	}

	const bool busy = s_steamUi.busy;
	const bool qrActive = s_steamUi.qrActive;

	ImGui::BeginDisabled(busy && !qrActive);

	ImGui::SeparatorText("Account");
	ImGui::InputText("Username", s_steamUi.username, IM_ARRAYSIZE(s_steamUi.username));
	ImGui::InputText("Password", s_steamUi.password, IM_ARRAYSIZE(s_steamUi.password), ImGuiInputTextFlags_Password);
	ImGui::Checkbox("Remember login", &s_steamUi.rememberLogin);

	if (s_steamUi.awaitingGuard)
	{
		ImGui::TextWrapped("%s", s_steamUi.guardPrompt.c_str());
		ImGui::InputText("Steam Guard code", s_steamUi.guardCode, IM_ARRAYSIZE(s_steamUi.guardCode));
		if (ImGui::Button("Submit code"))
		{
			std::lock_guard lock(s_steamUi.guardMutex);
			s_steamUi.guardReady = true;
			s_steamUi.guardCancelled = false;
			s_steamUi.guardCv.notify_all();
			s_steamUi.awaitingGuard = false;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel guard"))
		{
			std::lock_guard lock(s_steamUi.guardMutex);
			s_steamUi.guardReady = true;
			s_steamUi.guardCancelled = true;
			s_steamUi.guardCv.notify_all();
			s_steamUi.awaitingGuard = false;
		}
	}
	else if (!qrActive && ImGui::Button("Login"))
	{
		s_steamUi.busy = true;
		SteamStatus("Logging in...");
		CThread([]()
			{
				std::string error;
				const bool ok = g_steamClient.Login(s_steamUi.username, s_steamUi.password, s_steamUi.rememberLogin,
					[](const std::string& prompt) -> std::string
					{
						{
							std::lock_guard lock(s_steamUi.guardMutex);
							s_steamUi.guardPrompt = prompt;
							s_steamUi.guardCode[0] = '\0';
							s_steamUi.guardReady = false;
							s_steamUi.guardCancelled = false;
							s_steamUi.awaitingGuard = true;
						}
						std::unique_lock lock(s_steamUi.guardMutex);
						s_steamUi.guardCv.wait(lock, [] { return s_steamUi.guardReady; });
						if (s_steamUi.guardCancelled)
							return {};
						return s_steamUi.guardCode;
					}, error);

				if (!ok)
					SteamStatus("Login failed: " + error);
				else
					SteamStatus("Logged in");
				s_steamUi.busy = false;
			}).detach();
	}

	if (!qrActive)
	{
		ImGui::SameLine();
		if (ImGui::Button("Login with QR"))
			StartQrLogin();

		ImGui::SameLine();
		if (ImGui::Button("Anonymous login"))
		{
			s_steamUi.busy = true;
			CThread([]()
				{
					std::string error;
					if (!g_steamClient.LoginAnonymous(error))
						SteamStatus("Anonymous login failed: " + error);
					else
						SteamStatus("Logged in anonymously");
					s_steamUi.busy = false;
				}).detach();
		}
	}

	ImGui::EndDisabled();

	if (qrActive)
	{
		ImGui::SeparatorText("Steam QR login");
		if (s_steamUi.qrAwaitingConfirm)
			ImGui::TextWrapped("Confirm the login in your Steam mobile app.");
		else
			ImGui::TextWrapped("Scan this QR code with the Steam mobile app.");

		bool hasUrl = false;
		{
			std::lock_guard lock(s_steamUi.qrMutex);
			hasUrl = !s_steamUi.qrUrl.empty();
		}
		if (hasUrl)
			DrawQrCodeWidget();
		else if (!s_steamUi.qrAwaitingConfirm)
			ImGui::TextUnformatted("Requesting QR...");

		if (ImGui::Button("Cancel QR login"))
			s_steamUi.qrCancel.store(true);
	}

	ImGui::BeginDisabled(busy);

	ImGui::SeparatorText("Depot");
	ImGui::InputText("App ID", s_steamUi.appId, IM_ARRAYSIZE(s_steamUi.appId));
	ImGui::InputText("Depot ID", s_steamUi.depotId, IM_ARRAYSIZE(s_steamUi.depotId));
	ImGui::InputText("Branch", s_steamUi.branch, IM_ARRAYSIZE(s_steamUi.branch));
	ImGui::InputText("Manifest ID (empty = latest)", s_steamUi.manifestId, IM_ARRAYSIZE(s_steamUi.manifestId));

	if (ImGui::Button("Pin depot / load manifest"))
	{
		s_steamUi.busy = true;
		SteamStatus("Resolving depot manifest...");
		CThread([]()
			{
				std::string error;
				const uint32_t appId = static_cast<uint32_t>(strtoul(s_steamUi.appId, nullptr, 10));
				const uint32_t depotId = static_cast<uint32_t>(strtoul(s_steamUi.depotId, nullptr, 10));
				const uint64_t manifestId = s_steamUi.manifestId[0] ? strtoull(s_steamUi.manifestId, nullptr, 10) : 0ull;

				if (!g_steamClient.SetDepotContext(appId, depotId, s_steamUi.branch, manifestId, error))
				{
					SteamStatus("Failed to pin depot: " + error);
					s_steamUi.busy = false;
					return;
				}

				std::vector<SteamDepotFile_t> files;
				if (!g_steamClient.ListManifestFiles(files, error))
				{
					SteamStatus("Failed to list manifest: " + error);
					s_steamUi.busy = false;
					return;
				}

				s_steamUi.rpakFiles.clear();
				for (const auto& file : files)
				{
					if (file.depotPath.ends_with(".rpak"))
						s_steamUi.rpakFiles.push_back(file);
				}
				s_steamUi.selected.assign(s_steamUi.rpakFiles.size(), false);

				const auto& ctx = g_steamClient.GetDepotContext();
				snprintf(s_steamUi.manifestId, IM_ARRAYSIZE(s_steamUi.manifestId), "%llu",
					static_cast<unsigned long long>(ctx.manifestId));
				SteamStatus(std::format("Pinned depot {} manifest {} ({} rpaks)", ctx.depotId, ctx.manifestId, s_steamUi.rpakFiles.size()));
				s_steamUi.busy = false;
			}).detach();
	}

	ImGui::SeparatorText("RPak files");
	ImGui::InputText("Filter", s_steamUi.filter, IM_ARRAYSIZE(s_steamUi.filter));

	if (ImGui::BeginChild("rpak_list", ImVec2(0, -60), ImGuiChildFlags_Borders))
	{
		for (size_t i = 0; i < s_steamUi.rpakFiles.size(); ++i)
		{
			const auto& file = s_steamUi.rpakFiles[i];
			if (s_steamUi.filter[0] != '\0' && file.depotPath.find(s_steamUi.filter) == std::string::npos)
				continue;

			bool selected = s_steamUi.selected[i];
			if (ImGui::Checkbox(file.depotPath.c_str(), &selected))
				s_steamUi.selected[i] = selected;
		}
	}
	ImGui::EndChild();

	if (ImGui::Button("Download & load selected"))
	{
		std::vector<std::string> selectedPaths;
		for (size_t i = 0; i < s_steamUi.rpakFiles.size(); ++i)
		{
			if (s_steamUi.selected[i])
				selectedPaths.push_back(s_steamUi.rpakFiles[i].depotPath);
		}

		if (selectedPaths.empty())
		{
			SteamStatus("No rpaks selected");
		}
		else
		{
			s_steamUi.busy = true;
			SteamStatus("Downloading rpaks...");
			CThread([paths = std::move(selectedPaths)]()
				{
					std::vector<std::string> localPaths;
					std::string error;
					std::atomic<uint32_t> progress = 0;
					const ProgressBarEvent_t* bar = g_pImGuiHandler->AddProgressBarEvent("Steam download..",
						static_cast<uint32_t>(paths.size()), &progress, true);

					const bool ok = SteamDownloadRpaksForLoad(paths, localPaths, error, &progress);
					g_pImGuiHandler->FinishProgressBarEvent(bar);

					if (!ok)
					{
						SteamStatus("Download failed: " + error);
						s_steamUi.busy = false;
						return;
					}

					SteamStatus(std::format("Loading {} rpaks...", localPaths.size()));
					Bridge_HandleLoad(std::move(localPaths));
					SteamStatus("Done");
					s_steamUi.busy = false;
				}).detach();
		}
	}

	ImGui::SameLine();
	if (ImGui::Button("Clear Steam cache"))
	{
		g_steamCacheManager.Clear();
		SteamStatus("Steam cache cleared");
	}

	ImGui::EndDisabled();

	if (!s_steamUi.status.empty())
		ImGui::TextWrapped("%s", s_steamUi.status.c_str());

	if (g_steamClient.HasDepotContext())
	{
		const auto& ctx = g_steamClient.GetDepotContext();
		ImGui::Text("Active: app %u depot %u manifest %llu", ctx.appId, ctx.depotId,
			static_cast<unsigned long long>(ctx.manifestId));
	}

	ImGui::End();
}

void RenderSteamLoadWindow()
{
	DrawSteamLoadWindow();
}

#else // RTECH_STATIC_LIB

void OpenSteamLoadWindow() {}
void RenderSteamLoadWindow() {}

#endif // !RTECH_STATIC_LIB
