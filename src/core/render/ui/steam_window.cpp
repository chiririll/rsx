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
#include <core/steam/steam_depot_util.h>
#include <core/filehandling/load.h>

enum class SteamWizardStep : int
{
	Auth = 0,
	GameDepot,
	ManifestFiles,
};

enum class SteamGamePreset : int
{
	Apex = 0,
	Titanfall2,
	Custom,
};

enum class SteamManifestMode : int
{
	Latest = 0,
	Custom,
};

struct SteamWindowState_t
{
	bool open = false;
	bool restoreAttempted = false;
	SteamWizardStep step = SteamWizardStep::Auth;

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

	SteamGamePreset gamePreset = SteamGamePreset::Apex;
	char appId[32]{ "1172470" };
	char depotId[32]{};
	char branch[64]{ "public" };
	char manifestId[64]{}; // used when ManifestMode::Custom; empty Latest = 0

	SteamManifestMode manifestMode = SteamManifestMode::Latest;

	std::vector<SteamDepotInfo_t> depotInfos;
	std::vector<SteamBranchInfo_t> branchInfos;
	int selectedDepotIndex = -1;
	int selectedBranchIndex = -1;

	// Touched from worker threads and the UI frame — always take listMutex.
	std::mutex listMutex;
	std::vector<SteamDepotFile_t> rpakFiles;
	std::vector<bool> selected;
	char filter[128]{};

	std::string status;
	bool busy = false;
};

static SteamWindowState_t s_steamUi;

static constexpr const char* kWizardStepLabels[] = {
	"A · Auth",
	"B · Game / Depot",
	"C · Manifest / Files",
};

static void SteamStatus(const std::string& msg)
{
	s_steamUi.status = msg;
	Log("STEAM: %s\n", msg.c_str());
}

static void ApplyGamePreset(SteamGamePreset preset)
{
	s_steamUi.gamePreset = preset;
	switch (preset)
	{
	case SteamGamePreset::Apex:
		strncpy_s(s_steamUi.appId, "1172470", _TRUNCATE);
		break;
	case SteamGamePreset::Titanfall2:
		strncpy_s(s_steamUi.appId, "1237970", _TRUNCATE);
		break;
	case SteamGamePreset::Custom:
		break;
	}
}

static void ApplyDepotSelection(int index)
{
	if (index < 0 || index >= static_cast<int>(s_steamUi.depotInfos.size()))
		return;

	const SteamDepotInfo_t& info = s_steamUi.depotInfos[static_cast<size_t>(index)];
	s_steamUi.selectedDepotIndex = index;
	snprintf(s_steamUi.depotId, IM_ARRAYSIZE(s_steamUi.depotId), "%u", info.depotId);
}

static void ApplyBranchSelection(int index)
{
	if (index < 0 || index >= static_cast<int>(s_steamUi.branchInfos.size()))
		return;

	s_steamUi.selectedBranchIndex = index;
	strncpy_s(s_steamUi.branch, s_steamUi.branchInfos[static_cast<size_t>(index)].name.c_str(), _TRUNCATE);
}

static void ClearFileState()
{
	{
		std::lock_guard lock(s_steamUi.listMutex);
		s_steamUi.rpakFiles.clear();
		s_steamUi.selected.clear();
	}
	g_steamClient.ClearDepotContext();
}

static void ClearDepotAndFileState(bool clearBranches = true)
{
	s_steamUi.depotInfos.clear();
	s_steamUi.selectedDepotIndex = -1;
	s_steamUi.depotId[0] = '\0';
	if (clearBranches)
	{
		s_steamUi.branchInfos.clear();
		s_steamUi.selectedBranchIndex = -1;
	}
	ClearFileState();
}

static void AdvanceToGameDepotIfSignedIn()
{
	if (g_steamClient.IsSignedIn() && s_steamUi.step == SteamWizardStep::Auth
		&& !s_steamUi.awaitingGuard && !s_steamUi.qrActive)
	{
		s_steamUi.step = SteamWizardStep::GameDepot;
	}
}

static void TryRestoreSessionAsync()
{
	if (s_steamUi.restoreAttempted || s_steamUi.busy || g_steamClient.IsSignedIn())
	{
		AdvanceToGameDepotIfSignedIn();
		return;
	}

	if (!g_steamClient.HasRememberedToken())
	{
		s_steamUi.restoreAttempted = true;
		return;
	}

	s_steamUi.restoreAttempted = true;
	s_steamUi.busy = true;
	SteamStatus("Restoring saved Steam session...");

	CThread([]()
		{
			std::string error;
			if (g_steamClient.TryRestoreSession(error))
			{
				SteamStatus("Restored Steam session"
					+ (g_steamClient.GetUsername().empty()
						? std::string{}
						: (" as " + g_steamClient.GetUsername())));
				const std::string user = g_steamClient.GetRememberedUsername();
				if (!user.empty())
					strncpy_s(s_steamUi.username, user.c_str(), _TRUNCATE);
				s_steamUi.step = SteamWizardStep::GameDepot;
			}
			else
			{
				SteamStatus("Saved login expired — scan QR or sign in again (" + error + ")");
			}
			s_steamUi.busy = false;
		}).detach();
}

void OpenSteamLoadWindow()
{
	s_steamUi.open = true;
	if (s_steamUi.username[0] == '\0')
	{
		const std::string remembered = g_steamClient.GetRememberedUsername();
		if (!remembered.empty())
			strncpy_s(s_steamUi.username, remembered.c_str(), _TRUNCATE);
	}

	if (g_steamClient.IsSignedIn())
		s_steamUi.step = SteamWizardStep::GameDepot;
	else
		s_steamUi.step = SteamWizardStep::Auth;

	TryRestoreSessionAsync();
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
				s_steamUi.step = SteamWizardStep::GameDepot;
			}
			s_steamUi.busy = false;
		}).detach();
}

static void DrawWizardHeader()
{
	ImGui::TextUnformatted("Load from Steam");
	ImGui::Spacing();

	for (int i = 0; i < 3; ++i)
	{
		if (i > 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled(" > ");
			ImGui::SameLine();
		}

		const bool current = static_cast<int>(s_steamUi.step) == i;
		const bool done = static_cast<int>(s_steamUi.step) > i;
		if (current)
			ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.f, 1.f), "%s", kWizardStepLabels[i]);
		else if (done)
			ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "%s", kWizardStepLabels[i]);
		else
			ImGui::TextDisabled("%s", kWizardStepLabels[i]);
	}

	ImGui::Separator();
}

static void DrawStageAuth(bool busy, bool qrActive)
{
	ImGui::BeginDisabled(busy && !qrActive);

	if (g_steamClient.IsSignedIn())
	{
		if (g_steamClient.IsAnonymous())
		{
			ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f),
				"Signed in anonymously (cannot decrypt owned depots)");
		}
		else
		{
			const std::string signedInLabel = g_steamClient.GetUsername().empty()
				? std::string("Signed in")
				: ("Signed in as " + g_steamClient.GetUsername());
			ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "%s", signedInLabel.c_str());
		}

		if (!qrActive && !s_steamUi.awaitingGuard && ImGui::Button("Logout"))
		{
			g_steamClient.Logout();
			ClearDepotAndFileState();
			s_steamUi.step = SteamWizardStep::Auth;
			SteamStatus("Logged out");
		}
	}
	else
	{
		ImGui::TextDisabled("Not signed in");
	}

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
				{
					SteamStatus("Logged in");
					s_steamUi.step = SteamWizardStep::GameDepot;
				}
				s_steamUi.busy = false;
			}).detach();
	}

	if (!qrActive && !s_steamUi.awaitingGuard)
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
					{
						SteamStatus("Logged in anonymously (owned depots will not decrypt)");
						s_steamUi.step = SteamWizardStep::GameDepot;
					}
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
}

static void QueryDepotsAsync()
{
	s_steamUi.busy = true;
	SteamStatus("Querying app depots...");
	CThread([]()
		{
			std::string error;
			const uint32_t appId = static_cast<uint32_t>(strtoul(s_steamUi.appId, nullptr, 10));
			std::vector<SteamDepotInfo_t> depots;
			std::vector<SteamBranchInfo_t> branches;
			if (!g_steamClient.QueryAppDepotsAndBranches(appId, s_steamUi.branch, depots, branches, error))
			{
				SteamStatus("Query depots failed: " + error);
				s_steamUi.busy = false;
				return;
			}

			s_steamUi.depotInfos = std::move(depots);
			s_steamUi.branchInfos = std::move(branches);

			const int preferred = PickPreferredDepotIndex(s_steamUi.depotInfos);
			ApplyDepotSelection(preferred >= 0 ? preferred : 0);

			s_steamUi.selectedBranchIndex = -1;
			for (int i = 0; i < static_cast<int>(s_steamUi.branchInfos.size()); ++i)
			{
				if (s_steamUi.branchInfos[static_cast<size_t>(i)].name == s_steamUi.branch)
				{
					s_steamUi.selectedBranchIndex = i;
					break;
				}
			}

			SteamStatus(std::format("Found {} depots; selected depot {}",
				s_steamUi.depotInfos.size(),
				s_steamUi.depotId[0] ? s_steamUi.depotId : "?"));
			s_steamUi.busy = false;
		}).detach();
}

static void DrawStageGameDepot(bool busy)
{
	ImGui::BeginDisabled(busy);

	if (g_steamClient.IsSignedIn())
	{
		if (g_steamClient.IsAnonymous())
		{
			ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f),
				"Anonymous session — depot query requires a real Steam login.");
		}
		else
		{
			const std::string signedInLabel = g_steamClient.GetUsername().empty()
				? std::string("Signed in")
				: ("Signed in as " + g_steamClient.GetUsername());
			ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "%s", signedInLabel.c_str());
		}

		ImGui::SameLine();
		if (ImGui::SmallButton("Logout"))
		{
			g_steamClient.Logout();
			ClearDepotAndFileState();
			s_steamUi.step = SteamWizardStep::Auth;
			SteamStatus("Logged out");
		}
	}

	ImGui::SeparatorText("Game");

	int preset = static_cast<int>(s_steamUi.gamePreset);
	if (ImGui::RadioButton("Apex Legends", &preset, static_cast<int>(SteamGamePreset::Apex)))
	{
		ApplyGamePreset(SteamGamePreset::Apex);
		ClearDepotAndFileState();
	}
	ImGui::SameLine();
	if (ImGui::RadioButton("Titanfall 2", &preset, static_cast<int>(SteamGamePreset::Titanfall2)))
	{
		ApplyGamePreset(SteamGamePreset::Titanfall2);
		ClearDepotAndFileState();
	}
	ImGui::SameLine();
	if (ImGui::RadioButton("Custom App ID", &preset, static_cast<int>(SteamGamePreset::Custom)))
	{
		ApplyGamePreset(SteamGamePreset::Custom);
		ClearDepotAndFileState();
	}

	if (s_steamUi.gamePreset == SteamGamePreset::Custom)
		ImGui::InputText("App ID", s_steamUi.appId, IM_ARRAYSIZE(s_steamUi.appId));
	else
		ImGui::Text("App ID: %s", s_steamUi.appId);

	ImGui::SeparatorText("Depot");

	const bool canQuery = g_steamClient.IsSignedIn() && !g_steamClient.IsAnonymous();
	ImGui::BeginDisabled(!canQuery);
	if (ImGui::Button("Query depots"))
		QueryDepotsAsync();
	ImGui::EndDisabled();
	if (!canQuery)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(sign in with account or QR)");
	}

	if (!s_steamUi.depotInfos.empty())
	{
		std::string preview = s_steamUi.selectedDepotIndex >= 0
			? std::format("{} ({})",
				s_steamUi.depotInfos[static_cast<size_t>(s_steamUi.selectedDepotIndex)].depotId,
				s_steamUi.depotInfos[static_cast<size_t>(s_steamUi.selectedDepotIndex)].name.empty()
					? "unnamed"
					: s_steamUi.depotInfos[static_cast<size_t>(s_steamUi.selectedDepotIndex)].name)
			: "Select depot";

		if (ImGui::BeginCombo("Depot", preview.c_str()))
		{
			for (int i = 0; i < static_cast<int>(s_steamUi.depotInfos.size()); ++i)
			{
				const SteamDepotInfo_t& info = s_steamUi.depotInfos[static_cast<size_t>(i)];
				const bool selected = i == s_steamUi.selectedDepotIndex;
				const std::string label = std::format("{}  {}  man={}  os={}",
					info.depotId,
					info.name.empty() ? "-" : info.name,
					info.branchManifestId,
					info.oslist.empty() ? "-" : info.oslist);
				if (ImGui::Selectable(label.c_str(), selected))
				{
					if (i != s_steamUi.selectedDepotIndex)
						ClearFileState();
					ApplyDepotSelection(i);
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		if (s_steamUi.selectedDepotIndex >= 0
			&& s_steamUi.selectedDepotIndex < static_cast<int>(s_steamUi.depotInfos.size()))
		{
			const SteamDepotInfo_t& info = s_steamUi.depotInfos[static_cast<size_t>(s_steamUi.selectedDepotIndex)];
			std::string detail = std::format("Depot {} — {} | OS: {} | Branch manifest: {}",
				info.depotId,
				info.name.empty() ? "unnamed" : info.name,
				info.oslist.empty() ? "-" : info.oslist,
				info.branchManifestId);
			if (info.depotFromApp != 0)
				detail += std::format(" | shared from app {}", info.depotFromApp);
			ImGui::TextWrapped("%s", detail.c_str());
		}
	}
	else
	{
		ImGui::TextDisabled("No depots loaded yet.");
	}

	if (ImGui::CollapsingHeader("Advanced: branch"))
	{
		ImGui::TextWrapped("Usually leave this on public. Non-public branches may require a password Steam does not expose here.");

		if (!s_steamUi.branchInfos.empty())
		{
			std::string branchPreview = s_steamUi.selectedBranchIndex >= 0
				? s_steamUi.branchInfos[static_cast<size_t>(s_steamUi.selectedBranchIndex)].name
				: (s_steamUi.branch[0] ? s_steamUi.branch : "public");

			if (ImGui::BeginCombo("Branch list", branchPreview.c_str()))
			{
				for (int i = 0; i < static_cast<int>(s_steamUi.branchInfos.size()); ++i)
				{
					const SteamBranchInfo_t& info = s_steamUi.branchInfos[static_cast<size_t>(i)];
					const bool selected = i == s_steamUi.selectedBranchIndex;
					const std::string label = std::format("{}{}  build={}{}",
						info.name,
						info.passwordRequired ? " [pwd]" : "",
						info.buildId,
						info.description.empty() ? "" : ("  " + info.description));
					if (ImGui::Selectable(label.c_str(), selected))
					{
						ApplyBranchSelection(i);
						ClearDepotAndFileState(false); // keep branch list; re-query depots for new branch
					}
					if (selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		}

		if (ImGui::InputText("Branch", s_steamUi.branch, IM_ARRAYSIZE(s_steamUi.branch)))
			s_steamUi.selectedBranchIndex = -1;
	}

	ImGui::EndDisabled();
}

static void PinManifestAsync()
{
	s_steamUi.busy = true;
	SteamStatus("Resolving depot manifest...");
	CThread([]()
		{
			std::string error;
			const uint32_t appId = static_cast<uint32_t>(strtoul(s_steamUi.appId, nullptr, 10));
			const uint32_t depotId = static_cast<uint32_t>(strtoul(s_steamUi.depotId, nullptr, 10));
			const uint64_t manifestId = (s_steamUi.manifestMode == SteamManifestMode::Custom && s_steamUi.manifestId[0])
				? strtoull(s_steamUi.manifestId, nullptr, 10)
				: 0ull;

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

			std::vector<SteamDepotFile_t> rpaks;
			for (const auto& file : files)
			{
				if (file.depotPath.ends_with(".rpak"))
					rpaks.push_back(file);
			}
			std::vector<bool> sel(rpaks.size(), false);
			const size_t rpakCount = rpaks.size();

			{
				std::lock_guard lock(s_steamUi.listMutex);
				s_steamUi.rpakFiles.swap(rpaks);
				s_steamUi.selected.swap(sel);
			}

			const auto& ctx = g_steamClient.GetDepotContext();
			snprintf(s_steamUi.depotId, IM_ARRAYSIZE(s_steamUi.depotId), "%u", ctx.depotId);
			snprintf(s_steamUi.manifestId, IM_ARRAYSIZE(s_steamUi.manifestId), "%llu",
				static_cast<unsigned long long>(ctx.manifestId));
			SteamStatus(std::format("Pinned depot {} manifest {} ({} rpaks)", ctx.depotId, ctx.manifestId, rpakCount));
			s_steamUi.busy = false;
		}).detach();
}

static void DrawStageManifestFiles(bool busy)
{
	ImGui::BeginDisabled(busy);

	ImGui::Text("App %s · Depot %s · Branch %s",
		s_steamUi.appId[0] ? s_steamUi.appId : "?",
		s_steamUi.depotId[0] ? s_steamUi.depotId : "?",
		s_steamUi.branch[0] ? s_steamUi.branch : "public");

	ImGui::SeparatorText("Manifest");

	int mode = static_cast<int>(s_steamUi.manifestMode);
	if (ImGui::RadioButton("Latest for branch", &mode, static_cast<int>(SteamManifestMode::Latest)))
		s_steamUi.manifestMode = SteamManifestMode::Latest;
	ImGui::SameLine();
	if (ImGui::RadioButton("Custom ID", &mode, static_cast<int>(SteamManifestMode::Custom)))
		s_steamUi.manifestMode = SteamManifestMode::Custom;

	if (s_steamUi.manifestMode == SteamManifestMode::Custom)
		ImGui::InputText("Manifest ID", s_steamUi.manifestId, IM_ARRAYSIZE(s_steamUi.manifestId));
	else if (g_steamClient.HasDepotContext())
	{
		const auto& ctx = g_steamClient.GetDepotContext();
		ImGui::TextDisabled("Resolved manifest: %llu", static_cast<unsigned long long>(ctx.manifestId));
	}
	else
	{
		ImGui::TextDisabled("Will resolve the latest manifest for the selected branch.");
	}

	if (ImGui::Button("Load manifest"))
		PinManifestAsync();

	ImGui::SeparatorText("RPak files");
	ImGui::InputText("Filter", s_steamUi.filter, IM_ARRAYSIZE(s_steamUi.filter));

	if (ImGui::BeginChild("rpak_list", ImVec2(0, -70), ImGuiChildFlags_Borders))
	{
		std::lock_guard lock(s_steamUi.listMutex);
		if (s_steamUi.selected.size() != s_steamUi.rpakFiles.size())
			s_steamUi.selected.assign(s_steamUi.rpakFiles.size(), false);

		if (s_steamUi.rpakFiles.empty())
			ImGui::TextDisabled("Load a manifest to list rpak files.");

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
		{
			std::lock_guard lock(s_steamUi.listMutex);
			if (s_steamUi.selected.size() != s_steamUi.rpakFiles.size())
				s_steamUi.selected.assign(s_steamUi.rpakFiles.size(), false);

			for (size_t i = 0; i < s_steamUi.rpakFiles.size(); ++i)
			{
				if (s_steamUi.selected[i])
					selectedPaths.push_back(s_steamUi.rpakFiles[i].depotPath);
			}
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
}

static void DrawWizardFooter(bool busy)
{
	const bool guardOrQr = s_steamUi.awaitingGuard || s_steamUi.qrActive;
	ImGui::BeginDisabled(busy || guardOrQr);

	const bool canBack = s_steamUi.step != SteamWizardStep::Auth;
	const bool canNext = (s_steamUi.step == SteamWizardStep::Auth && g_steamClient.IsSignedIn())
		|| (s_steamUi.step == SteamWizardStep::GameDepot
			&& s_steamUi.depotId[0] != '\0'
			&& g_steamClient.IsSignedIn()
			&& !g_steamClient.IsAnonymous());

	if (canBack)
	{
		if (ImGui::Button("Back"))
		{
			if (s_steamUi.step == SteamWizardStep::ManifestFiles)
				s_steamUi.step = SteamWizardStep::GameDepot;
			else if (s_steamUi.step == SteamWizardStep::GameDepot)
				s_steamUi.step = SteamWizardStep::Auth;
		}
		ImGui::SameLine();
	}

	if (s_steamUi.step != SteamWizardStep::ManifestFiles)
	{
		ImGui::BeginDisabled(!canNext);
		if (ImGui::Button("Next"))
		{
			if (s_steamUi.step == SteamWizardStep::Auth)
				s_steamUi.step = SteamWizardStep::GameDepot;
			else if (s_steamUi.step == SteamWizardStep::GameDepot)
				s_steamUi.step = SteamWizardStep::ManifestFiles;
		}
		ImGui::EndDisabled();
	}

	ImGui::EndDisabled();
}

static void DrawSteamLoadWindow()
{
	if (!s_steamUi.open)
		return;

	ImGui::SetNextWindowSize(ImVec2(760, 620), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Load from Steam", &s_steamUi.open))
	{
		ImGui::End();
		return;
	}

	const bool busy = s_steamUi.busy;
	const bool qrActive = s_steamUi.qrActive;

	DrawWizardHeader();

	switch (s_steamUi.step)
	{
	case SteamWizardStep::Auth:
		DrawStageAuth(busy, qrActive);
		break;
	case SteamWizardStep::GameDepot:
		DrawStageGameDepot(busy);
		break;
	case SteamWizardStep::ManifestFiles:
		DrawStageManifestFiles(busy);
		break;
	}

	ImGui::Spacing();
	DrawWizardFooter(busy);

	ImGui::Separator();
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
