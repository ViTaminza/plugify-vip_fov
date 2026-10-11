// vip_fov — Plugify port of the Pisex VIP module "VIP_FOV" / "[VIP] FOV"
// (the original's vip_fov.cpp, 185 lines).
//
// Behaviour kept identical:
//   * feature "FOV" (VIP_BOOL, SELECTABLE, callback OnSelect).
//   * VIP_OnClientLoaded clears the slot and parses the player's "FOV" group value into the list
//     of selectable values (comma separated, the original's std::getline semantics).
//   * VIP_OnPlayerSpawn: when the list is not empty, the FOV stored in the "FOV_Value" cookie is
//     applied (90 when the cookie is empty), with the original's network state change.
//   * OnSelect builds the Pisex menu (title "FOV_Title", one item per value, exit + back
//     buttons) and its callback: a selected item stores the value in the cookie, applies it and
//     re-opens the menu; the exit-back button (item 7) opens the VIP menu. The callback returns
//     false, like the original (the menu is not closed by the selection).
//
// Port notes:
//   * the menu is the pisexcompat builder (g_pMenus) over the s2sdk menu backends; a menu change
//     made from inside the callback (the re-open) is applied one frame later, which is the
//     documented behaviour of the ported menu layer.
//   * m_iDesiredFOV is written through the schema on the raw controller pointer
//     (CBasePlayerController::m_iDesiredFOV) with changeState = true, which is exactly the
//     original's field write plus its explicit state-change call.
//   * the original's std::stoi could throw on a non-numeric value (an empty list element, a
//     hand-edited cookie) - such a value is skipped here instead (a non-numeric cookie keeps the
//     player's FOV, a non-numeric menu item does nothing).

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>
#include <vip_api.hpp>

#include "fov_logic.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The original code's unqualified spellings (VIP_BOOL, SELECTABLE, ...).
using namespace vip;

constexpr const char* kLogTag = "VIP-FOV"; // GetLogTag() of the original
constexpr const char* kFeature = "FOV";
constexpr const char* kTitlePhrase = "FOV_Title";

IVIPApi* g_pVIPCore = &IVIPApi::Get();

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// The original's ErrorLog("[%s] ...", GetLogTag()).
void LogTaggedError(const std::string& message) {
	LogError(std::string("[") + kLogTag + "] " + message);
}

// The original's desired-FOV field write, plus its explicit state change on the controller.
void SetClientFov(int iSlot, int iFov) {
	// The controller is a real entity, so the handle-based schema API is used (the "2" pointer
	// variants are only for non-entity schema objects - components, game rules). The handle is
	// validated first so an invalid one cannot print a warning per call.
	if (!__s2sdk_PlayerSlotToEntHandle || !__s2sdk_IsValidEntHandle || !__s2sdk_SetEntSchema) {
		return;
	}
	// s2sdk::PlayerSlotToEntHandle reads the server's client list; it must only be called for a
	// connected slot with a controller (crash-class: a raw client query on an unvalidated slot).
	// The spawn path is already validated by the core (IsAlive one frame earlier), but the menu
	// callback path (a stored slot) has no live-client check of its own, so guard here.
	if (!pisex::IsClientReady(iSlot)) {
		return;
	}
	try {
		// The original's controller lookup; nothing to write without one.
		const int handle = s2sdk::PlayerSlotToEntHandle(iSlot);
		if (!s2sdk::IsValidEntHandle(handle)) {
			return;
		}
		// changeState = true keeps the original's explicit state change.
		s2sdk::SetEntSchema(handle, "CBasePlayerController", "m_iDesiredFOV", iFov, true, 0);
	} catch (...) {
	}
}

class FovPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// VIP_OnClientLoaded of the original.
	void OnClientLoaded(int iSlot, bool bIsVIP);
	// VIP_OnPlayerSpawn of the original.
	void OnPlayerSpawn(int iSlot, int iTeam, bool bIsVIP);
	// OnSelect of the original: opens the FOV menu (also used to re-open it after a selection).
	void OpenFovMenu(int iSlot);

	// The per-slot list of selectable values (g_FOV of the original).
	std::vector<std::string> m_fov[vip_fov::kMaxSlots];
};

FovPlugin g_plugin;

void FovPlugin::OnClientLoaded(int iSlot, bool bIsVIP) {
	if (iSlot < 0 || iSlot >= vip_fov::kMaxSlots) {
		return;
	}
	// The original cleared the slot's list and parsed the "FOV" feature value for a VIP.
	m_fov[iSlot].clear();
	if (!bIsVIP) {
		return;
	}
	const std::string value = g_pVIPCore->VIP_GetClientFeatureString(iSlot, kFeature);
	m_fov[iSlot] = vip_fov::ParseFovList(value);
}

void FovPlugin::OnPlayerSpawn(int iSlot, int /*iTeam*/, bool bIsVIP) {
	if (!bIsVIP || iSlot < 0 || iSlot >= vip_fov::kMaxSlots) {
		return;
	}
	if (m_fov[iSlot].empty()) {
		return;
	}
	// The "FOV_Value" cookie of the original, defaulted to 90 when it is empty.
	const std::string cookie = g_pVIPCore->VIP_GetClientCookie(iSlot, vip_fov::kCookieName);
	if (cookie.empty()) {
		SetClientFov(iSlot, vip_fov::kDefaultFov);
		return;
	}
	int iFov = 0;
	if (!vip_fov::TryParseFov(cookie, iFov)) {
		// The original's std::stoi threw here; a non-numeric cookie leaves the FOV alone instead.
		LogTaggedError(std::string("cookie ") + vip_fov::kCookieName + " holds a non-numeric value '" + cookie +
					   "' - FOV left unchanged");
		return;
	}
	SetClientFov(iSlot, iFov);
}

void FovPlugin::OpenFovMenu(int iSlot) {
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, g_pVIPCore->VIP_GetTranslate(kTitlePhrase));
	for (const std::string& element : m_fov[iSlot]) {
		pisex::g_pMenus->AddItemMenu(hMenu, element.c_str(), element.c_str());
	}
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [this](const char* szBack, const char* /*szFront*/, int iItem, int iSlotOfMenu) {
		switch (vip_fov::ActionForItem(iItem)) {
		case vip_fov::SelectAction::SetFov: {
			int iFov = 0;
			// A non-number is skipped (the original's std::stoi threw on it).
			if (!vip_fov::TryParseFov(szBack ? szBack : "", iFov)) {
				return;
			}
			// The selection is stored in the "FOV_Value" cookie (the original's strdup leaked).
			g_pVIPCore->VIP_SetClientCookie(iSlotOfMenu, vip_fov::kCookieName, szBack);
			SetClientFov(iSlotOfMenu, iFov);
			OpenFovMenu(iSlotOfMenu); // the original's OnSelect(iSlot, "FOV") re-open
			return;
		}
		case vip_fov::SelectAction::OpenVipMenu:
			// Item 7 (the exit-back button) opens the VIP menu instead.
			g_pVIPCore->VIP_OpenMenu(iSlotOfMenu);
			return;
		case vip_fov::SelectAction::Ignore:
			return;
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

plg::PluginResult FovPlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("vip_fov");

		// The original registered its client-loaded and spawn callbacks.
		g_pVIPCore->VIP_OnClientLoaded([this](int iSlot, bool bIsVIP) { OnClientLoaded(iSlot, bIsVIP); });
		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int iTeam, bool bIsVIP) {
			OnPlayerSpawn(iSlot, iTeam, bIsVIP);
		});

		// The original registered the "FOV" feature with its OnSelect callback.
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, SELECTABLE, [this](int iSlot, const char* /*szFeature*/) {
			if (iSlot < 0 || iSlot >= vip_fov::kMaxSlots) {
				return false;
			}
			OpenFovMenu(iSlot);
			return false; // the original returned false (its callback does not close the menu)
		});

		LogInfo(std::string("[") + kLogTag + "] ready");
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult FovPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll(); // menu requests queued by the callback and timer reschedules
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult FovPlugin::OnPluginEnd() {
	try {
		g_pVIPCore->VIP_OnClientLoadedUnregister();
		g_pVIPCore->VIP_OnPlayerSpawnUnregister();
		// The vip API has no feature unregister: re-registering the feature without callbacks
		// replaces the core's entry with null delegates, so the core keeps no pointer into this
		// library after it is unloaded (same value/feature type: the menu entry is unchanged).
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_BOOL, SELECTABLE);
		pisex::Shutdown();
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
