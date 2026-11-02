// vip_fov — Plugify port of the Pisex VIP module "VIP_FOV" / "[VIP] FOV"
// (the original's vip_fov.cpp, 185 lines).
//
// Behaviour kept identical:
//   * feature "FOV" (VIP_BOOL, SELECTABLE, callback OnSelect).
//   * VIP_OnClientLoaded clears the slot and parses the player's "FOV" group value into the list
//     of selectable values (comma separated, the original's std::getline semantics).
//   * VIP_OnPlayerSpawn: when the list is not empty, the FOV stored in the "FOV_Value" cookie is
//     applied (90 when the cookie is empty), with the original's network state change.
//   * OnSelect builds the FOV menu (title "FOV_Title", one item per value, exit + back buttons)
//     and its callback: a selected item stores the value in the cookie, applies it and re-opens
//     the menu; the exit-back button opens the VIP menu. The feature callback returns false, like
//     the original (the menu is not closed by the selection).
//
// Pure s2sdk, no pisexcompat (docs/PLUGIN_STANDARD.md §7). Every safety rule the old wrappers
// enforced lives in this plugin:
//   * guards.cpp — map gate (PITFALLS B1/B2), the slot/controller identity chain (C2), the usable
//     signed entity handle filter (C1), the verified OwnerIdentity (SteamID64 + controller
//     handle/generation) and the map-gated, success-only-cached schema offset (C8); SetDesiredFov()
//     is the original's m_iDesiredFOV write + SetStateChanged on the controller handle;
//   * menus.cpp — the native s2sdk menu lifecycle (see its header): verified owner on every queued
//     request, full binding-set gate, deferred creation/destroy on OnPluginUpdate, MenuId 0
//     reservation, cancel-confirmed map-end teardown and the hudmenu "!menu" type choice;
//   * fov_logic.hpp — the engine-free decisions shared with the test (list parsing, guarded stoi,
//     the Pisex item dispatch, page-to-button and the menu-type mapping).
//
// Port notes:
//   * VIP_OnClientLoaded is fired by the vip core from its async DB completion, without a map
//     readiness guard: FeatureString/IsSlotHuman answer empty while the map is down. Such a
//     callback therefore only marks the slot (m_pendingLoad) and never calls the vip core until
//     the map gate is open again; FlushPendingClientState() then re-captures the *current* owner
//     identity, re-checks VIP_IsClientVIP and re-reads the feature value - a stale occupant is
//     never trusted, and the list is not lost across the OnMapEnd window.
//   * OnPlayerSpawn is gated the same way (m_pendingSpawn): a spawn that arrives while the map is
//     torn down or before the list was re-read is replayed once the gate is open, with a fresh
//     identity/VIP check.
//   * SeedLoadedVips() runs once per plugin instance, on the first update with an open map: a
//     mid-map `plg reload` therefore re-reads the feature of every already-loaded VIP (identity
//     verified, VIP_IsClientVIP queried) without granting anything to a non-VIP.
//   * the original resolved std::stoi through the pisexcompat menu trampoline, so the callback
//     received Pisex item numbers 1..5 / 7 / 9. Native s2sdk passes the global item index and
//     accepts digits 6/7 for the next page; menus.cpp translates the index back into the visible
//     Pisex button (VisibleMenuButton) before acting, so the original dispatch is unchanged.
//   * m_iDesiredFOV is written through a cached raw schema offset (handle-based SetEntData, 4
//     bytes, changeState = true). The typed SetEntSchema route of the old port is avoided on
//     purpose (PITFALLS C6/C7: schema type poison and the CS2BadList silent no-ops).
//   * the original's std::stoi could throw on a non-numeric value (an empty list element, a
//     hand-edited cookie) - such a value is skipped here instead (a non-numeric cookie keeps the
//     player's FOV, a non-numeric menu item does nothing).
//   * the old port called the menu API from inside the VIP menu's handler (s2sdk's menu manager
//     mutex held). Native menus are created/destroyed only from OnPluginUpdate now; the engine
//     callback just records the selection, so no cookie, schema or menu call runs under that lock.
//   * OnPluginStart never returns Failed (PITFALLS A1), and OnPluginEnd symmetrically drops every
//     registration, menu, map listener and schema offset it owns (PITFALLS A7: `plg reload` is
//     safe). There are no console commands, convars, game-event hooks or timers in this module, so
//     command ownership / FindConVar ordering / timer-mutex / C13 GetEventPlayerSlot rules do not
//     apply (the vip core hands us a player slot, which is range- and identity-checked).

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>
#include <vip_api.hpp>

#include "common.hpp"
#include "fov_logic.hpp"
#include "guards.hpp"
#include "menus.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The original code's unqualified spellings (VIP_BOOL, SELECTABLE, ...).
using namespace vip;

// The vip core consumer wrapper (plugins/vip/include/vip_api.hpp): method names, argument order and
// the plain enums of the original's IVIPApi.
IVIPApi* g_pVIPCore = &IVIPApi::Get();

// The vip consumer wrapper gates only on __vip_VIP_IsVIPLoaded (detail::Available()), then calls the
// individual function-pointer slots directly. The plugify module fills all of them together in a
// normal install, but a partially failed/partial load would make those direct calls a null function
// pointer (a try/catch cannot catch that). Every call site below therefore checks its own binding
// first; the registration pairs additionally require BOTH Register and Unregister before anything is
// registered, and the plugin remembers what it really registered so OnPluginEnd only unregisters
// what exists (no dangling callback after `plg reload`).
bool VipCoreReady() {
	return __vip_VIP_IsVIPLoaded != nullptr;
}

bool VipClientLoadedPairReady() {
	return __vip_VIP_OnClientLoaded_Register != nullptr &&
		   __vip_VIP_OnClientLoaded_Unregister != nullptr;
}

bool VipSpawnPairReady() {
	return __vip_VIP_OnPlayerSpawn_Register != nullptr &&
		   __vip_VIP_OnPlayerSpawn_Unregister != nullptr;
}

void LogVipBindingGap(const char* what) {
	vip_fov::LogTaggedError(std::string("vip binding ") + what +
							" is unavailable - the affected FOV path stays disabled");
}

class FovPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// VIP_OnClientLoaded / VIP_OnPlayerSpawn of the original.
	void OnClientLoaded(int32_t iSlot, bool bIsVIP);
	void OnPlayerSpawn(int32_t iSlot, int32_t iTeam, bool bIsVIP);

	// The SELECTABLE feature callback of the original (OnSelect).
	void OnFeatureSelected(int32_t iSlot);

	// Applied from the queued menu action (ProcessMenuRequests, outside the menu manager's lock):
	// the original's menu callback body, on the verified owner of the request.
	void ApplySelection(const vip_fov::OwnerIdentity& owner, const std::string& value);
	void OpenVipMenu(const vip_fov::OwnerIdentity& owner);
	// Asked by the menu layer before CreateMenu/DisplayMenu and before every queued action.
	bool IsOwnerEligible(const vip_fov::OwnerIdentity& owner);

private:
	// Opens (or re-opens) the FOV menu; the actual CreateMenu/DisplayMenu happens one update later.
	void OpenFovMenu(const vip_fov::OwnerIdentity& owner);

	// The vip-core access, only ever called while HasController() is true.
	void ReadFeatureList(int32_t iSlot);
	void ApplyStoredCookie(int32_t iSlot);

	// Replays callbacks that arrived while the map gate was closed, and seeds the already-loaded
	// VIPs once per plugin instance (mid-map reload).
	void FlushPendingClientState();
	void SeedLoadedVips();

	// The per-slot list of selectable values (g_FOV of the original).
	std::vector<std::string> m_fov[vip_fov::kMaxSlots];
	// 1 = the vip core callback arrived without a safe map/controller, (re)read after the gate opens.
	std::array<uint8_t, vip_fov::kMaxSlots> m_pendingLoad{};
	std::array<uint8_t, vip_fov::kMaxSlots> m_pendingSpawn{};
	bool m_seeded = false;
	// What this library instance really registered; only that is unregistered in OnPluginEnd.
	bool m_clientLoadedRegistered = false;
	bool m_spawnRegistered = false;
	bool m_featureRegistered = false;
};

FovPlugin g_plugin;

// Menu-controller callbacks: s2sdk and the menu layer use plain function pointers, so the plugin
// instance is passed through the controller's user pointer. The owner comes from the menu layer
// and is re-validated by the plugin before any game/cookie action.
void MenuSelectTrampoline(void* user, const vip_fov::OwnerIdentity& owner, const std::string& value) {
	static_cast<FovPlugin*>(user)->ApplySelection(owner, value);
}

void MenuExitBackTrampoline(void* user, const vip_fov::OwnerIdentity& owner) {
	static_cast<FovPlugin*>(user)->OpenVipMenu(owner);
}

bool MenuOwnerEligibleTrampoline(void* user, const vip_fov::OwnerIdentity& owner) {
	return static_cast<FovPlugin*>(user)->IsOwnerEligible(owner);
}

void OnMapEndHook() {
	// From here until the next OnMapStart the client list is torn down: only menu-manager state is
	// touched, no client and no schema call (PITFALLS B2/C10).
	vip_fov::CloseMenusOnMapEnd();
}

void FovPlugin::ReadFeatureList(int32_t iSlot) {
	// Only called after HasController() succeeded in this tick (the vip core's GroupFeaturesForSlot
	// needs a live map/client, and we do not want that call in the OnMapEnd window).
	if (__vip_VIP_GetClientFeatureString == nullptr) {
		m_fov[iSlot].clear();
		return;
	}
	const char* szValue = g_pVIPCore->VIP_GetClientFeatureString(iSlot, vip_fov::kFeature);
	m_fov[iSlot] = szValue != nullptr ? vip_fov::ParseFovList(szValue) : std::vector<std::string>{};
}

void FovPlugin::ApplyStoredCookie(int32_t iSlot) {
	// The "FOV_Value" cookie of the original, defaulted to 90 when it is empty.
	if (__vip_VIP_GetClientCookie == nullptr) {
		return; // partial vip load: do not guess a FOV without the stored preference
	}
	const char* szCookie = g_pVIPCore->VIP_GetClientCookie(iSlot, vip_fov::kCookieName);
	const std::string cookie = szCookie != nullptr ? szCookie : "";
	if (cookie.empty()) {
		vip_fov::SetDesiredFov(iSlot, vip_fov::kDefaultFov);
		return;
	}
	int iFov = 0;
	if (!vip_fov::TryParseFov(cookie, iFov)) {
		// The original's std::stoi threw here; a non-numeric cookie leaves the FOV alone instead.
		vip_fov::LogTaggedError(std::string("cookie ") + vip_fov::kCookieName +
								" holds a non-numeric value '" + cookie + "' - FOV left unchanged");
		return;
	}
	vip_fov::SetDesiredFov(iSlot, iFov);
}

void FovPlugin::OnClientLoaded(int32_t iSlot, bool bIsVIP) {
	if (!vip_fov::IsSlotInRange(iSlot)) {
		return;
	}
	// The original cleared the slot's list and parsed the "FOV" feature value for a VIP. The clear
	// happens even when the re-read has to be deferred, so a stale list is never used meanwhile.
	m_fov[iSlot].clear();
	m_pendingSpawn[iSlot] = 0;
	if (!bIsVIP) {
		m_pendingLoad[iSlot] = 0;
		return;
	}
	if (!vip_fov::HasController(iSlot)) {
		// The vip core fires this callback from its async DB completion without a map guard, and
		// FeatureString answers empty while the map is down. Remember the slot and re-read it after
		// OnMapStart, with the then-current identity/VIP state (never a stale occupant).
		m_pendingLoad[iSlot] = 1;
		return;
	}
	m_pendingLoad[iSlot] = 0;
	ReadFeatureList(iSlot);
}

void FovPlugin::OnPlayerSpawn(int32_t iSlot, int32_t /*iTeam*/, bool bIsVIP) {
	if (!vip_fov::IsSlotInRange(iSlot)) {
		return;
	}
	m_pendingSpawn[iSlot] = 0;
	if (!bIsVIP) {
		return;
	}
	if (m_pendingLoad[iSlot] != 0 || !vip_fov::HasController(iSlot)) {
		// No client/schema/vip-core call in the changelevel window; replay after the gate opens.
		m_pendingSpawn[iSlot] = 1;
		return;
	}
	if (m_fov[iSlot].empty()) {
		return;
	}
	ApplyStoredCookie(iSlot);
}

void FovPlugin::FlushPendingClientState() {
	for (int32_t iSlot = 0; iSlot < vip_fov::kMaxSlots; ++iSlot) {
		if (m_pendingLoad[iSlot] == 0 && m_pendingSpawn[iSlot] == 0) {
			continue;
		}
		// The current occupant must be a verified owner right now; a slot that was re-used while
		// the request waited is never served with the old player's state.
		vip_fov::OwnerIdentity owner;
		if (!vip_fov::CaptureOwner(iSlot, owner)) {
			continue; // still no live map/controller: retry on the next update
		}
		bool bIsVip = false;
		if (__vip_VIP_IsClientVIP != nullptr) {
			try {
				bIsVip = g_pVIPCore->VIP_IsClientVIP(owner.slot);
			} catch (...) {
				bIsVip = false;
			}
		}
		if (m_pendingLoad[iSlot] != 0) {
			m_pendingLoad[iSlot] = 0;
			if (bIsVip) {
				ReadFeatureList(owner.slot);
			} else {
				m_fov[iSlot].clear();
			}
		}
		if (m_pendingSpawn[iSlot] != 0) {
			m_pendingSpawn[iSlot] = 0;
			if (bIsVip && !m_fov[iSlot].empty()) {
				ApplyStoredCookie(owner.slot);
			}
		}
	}
}

void FovPlugin::SeedLoadedVips() {
	// A mid-map `plg reload` re-registers the callbacks but the vip core does not re-fire
	// VIP_OnClientLoaded for the players it already has. Read the feature of every still-connected
	// human VIP once, behind a fresh identity + VIP check, so their list is not empty after the
	// reload. Non-VIPs are only checked for identity; nothing is written or registered for them.
	for (int32_t iSlot = 0; iSlot < vip_fov::kMaxSlots; ++iSlot) {
		if (m_pendingLoad[iSlot] != 0 || !m_fov[iSlot].empty()) {
			continue; // a queued callback will fill it, or it is already populated
		}
		vip_fov::OwnerIdentity owner;
		if (!vip_fov::CaptureOwner(iSlot, owner)) {
			continue;
		}
		bool bIsVip = false;
		if (__vip_VIP_IsClientVIP != nullptr) {
			try {
				bIsVip = g_pVIPCore->VIP_IsClientVIP(owner.slot);
			} catch (...) {
				bIsVip = false;
			}
		}
		if (bIsVip) {
			ReadFeatureList(owner.slot);
		}
	}
}

void FovPlugin::OnFeatureSelected(int32_t iSlot) {
	if (!vip_fov::IsSlotInRange(iSlot)) {
		return;
	}
	// Verified owner at request time; the menu layer re-checks it (and the VIP entitlement) before
	// CreateMenu/DisplayMenu, so a slot re-used in between never sees the menu.
	vip_fov::OwnerIdentity owner;
	if (!vip_fov::CaptureOwner(iSlot, owner)) {
		return;
	}
	OpenFovMenu(owner);
}

void FovPlugin::OpenFovMenu(const vip_fov::OwnerIdentity& owner) {
	if (!owner.IsSet() || !vip_fov::IsMapReady()) {
		return;
	}
	vip_fov::MenuPlan plan;
	const char* szTitle = __vip_VIP_GetTranslate != nullptr
							  ? g_pVIPCore->VIP_GetTranslate(vip_fov::kTitlePhrase)
							  : nullptr;
	plan.title = szTitle != nullptr ? szTitle : "";
	plan.values = m_fov[owner.slot];
	vip_fov::DisplayMenuDeferred(owner, std::move(plan));
}

void FovPlugin::ApplySelection(const vip_fov::OwnerIdentity& owner, const std::string& value) {
	// Re-validate the verified owner and the VIP entitlement when the queued action runs: the
	// player may have left, the slot may have been re-used or the VIP status may have expired.
	if (!owner.IsSet() || !vip_fov::IsMapReady() || !vip_fov::OwnerMatches(owner) ||
		__vip_VIP_IsClientVIP == nullptr || !g_pVIPCore->VIP_IsClientVIP(owner.slot)) {
		return;
	}
	int iFov = 0;
	// A non-number is skipped (the original's std::stoi threw on it: an empty list element or a
	// hand-edited item).
	if (!vip_fov::TryParseFov(value, iFov)) {
		return;
	}
	// The selection is stored in the "FOV_Value" cookie (the original's strdup leaked). A missing
	// cookie setter only skips the persistence; the FOV itself is still applied.
	if (__vip_VIP_SetClientCookie != nullptr) {
		g_pVIPCore->VIP_SetClientCookie(owner.slot, vip_fov::kCookieName, value.c_str());
	}
	vip_fov::SetDesiredFov(owner.slot, iFov);
	OpenFovMenu(owner); // the original's OnSelect(iSlot, "FOV") re-open
}

void FovPlugin::OpenVipMenu(const vip_fov::OwnerIdentity& owner) {
	// Same re-validation as ApplySelection(): a queued back action must not reach the vip core for
	// a client that is gone or a slot that now belongs to someone else.
	if (!owner.IsSet() || !vip_fov::IsMapReady() || !vip_fov::OwnerMatches(owner) ||
		__vip_VIP_IsClientVIP == nullptr || __vip_VIP_OpenMenu == nullptr ||
		!g_pVIPCore->VIP_IsClientVIP(owner.slot)) {
		return;
	}
	g_pVIPCore->VIP_OpenMenu(owner.slot); // the original's item 7 (exit-back) branch
}

bool FovPlugin::IsOwnerEligible(const vip_fov::OwnerIdentity& owner) {
	if (!vip_fov::OwnerMatches(owner) || __vip_VIP_IsClientVIP == nullptr) {
		return false;
	}
	return g_pVIPCore->VIP_IsClientVIP(owner.slot);
}

plg::PluginResult FovPlugin::OnPluginStart() {
	try {
		// The map listeners gate every client/entity/schema call this plugin makes and close the
		// menus at map end; registering them touches neither a client nor an entity (PITFALLS B1).
		vip_fov::SetMapLifecycleCallbacks(nullptr, &OnMapEndHook);
		vip_fov::InitMapGate();

		// Requires the complete menu lifecycle binding set; burns MenuId 0 only when it is there.
		vip_fov::StartMenus();
		vip_fov::SetMenuHandlers(&g_plugin, &MenuSelectTrampoline, &MenuExitBackTrampoline,
								 &MenuOwnerEligibleTrampoline);

		m_seeded = false;
		for (int32_t iSlot = 0; iSlot < vip_fov::kMaxSlots; ++iSlot) {
			m_pendingLoad[iSlot] = 0;
			m_pendingSpawn[iSlot] = 0;
		}

		m_clientLoadedRegistered = false;
		m_spawnRegistered = false;
		m_featureRegistered = false;
		if (!VipCoreReady()) {
			vip_fov::LogTaggedError("the vip core bindings are unavailable - the FOV feature stays disabled");
		} else {
			// The wrapper's Available() only checks IsVIPLoaded and would dereference a null
			// Register/Unregister slot; require the whole pair before registering anything.
			if (VipClientLoadedPairReady()) {
				g_pVIPCore->VIP_OnClientLoaded([this](int32_t iSlot, bool bIsVIP) {
					// Nothing may unwind into the vip core's dispatch.
					try {
						OnClientLoaded(iSlot, bIsVIP);
					} catch (const std::exception& ex) {
						vip_fov::LogTaggedError(std::string("OnClientLoaded failed: ") + ex.what());
					} catch (...) {
						vip_fov::LogTaggedError("OnClientLoaded failed with an unknown exception");
					}
				});
				m_clientLoadedRegistered = true;
			} else {
				LogVipBindingGap("VIP_OnClientLoaded_Register/Unregister");
			}
			if (VipSpawnPairReady()) {
				g_pVIPCore->VIP_OnPlayerSpawn([this](int32_t iSlot, int32_t iTeam, bool bIsVIP) {
					try {
						OnPlayerSpawn(iSlot, iTeam, bIsVIP);
					} catch (const std::exception& ex) {
						vip_fov::LogTaggedError(std::string("OnPlayerSpawn failed: ") + ex.what());
					} catch (...) {
						vip_fov::LogTaggedError("OnPlayerSpawn failed with an unknown exception");
					}
				});
				m_spawnRegistered = true;
			} else {
				LogVipBindingGap("VIP_OnPlayerSpawn_Register/Unregister");
			}
			if (__vip_VIP_RegisterFeature != nullptr) {
				// The original registered the "FOV" feature with its OnSelect callback; the
				// callback returns false there too (its return value does not close the VIP menu).
				g_pVIPCore->VIP_RegisterFeature(vip_fov::kFeature, VIP_BOOL, SELECTABLE,
												[this](int32_t iSlot, const char* /*szFeature*/) {
													try {
														OnFeatureSelected(iSlot);
													} catch (const std::exception& ex) {
														vip_fov::LogTaggedError(std::string("OnSelect failed: ") + ex.what());
													} catch (...) {
														vip_fov::LogTaggedError("OnSelect failed with an unknown exception");
													}
													return false;
												});
				m_featureRegistered = true;
			} else {
				LogVipBindingGap("VIP_RegisterFeature");
			}
		}

		vip_fov::LogInfo(std::string("[") + vip_fov::kLogTag + "] ready");
	} catch (const std::exception& ex) {
		vip_fov::LogTaggedError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		vip_fov::LogTaggedError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin (PITFALLS A1)
}

plg::PluginResult FovPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		// Opens the gate on the first update after a mid-map load; after OnMapEnd only a new
		// OnMapStart opens it again (PITFALLS B1/B2).
		vip_fov::PollMapGate();
		if (vip_fov::IsMapReady()) {
			if (!m_seeded) {
				m_seeded = true;
				SeedLoadedVips(); // mid-map reload: re-read the still-connected VIPs once
			}
			FlushPendingClientState(); // callbacks that arrived in the OnMapEnd window
		}
		// Queued displays, selections and menu destroys - never inside a menu handler.
		vip_fov::ProcessMenuRequests();
	} catch (const std::exception& ex) {
		vip_fov::LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		vip_fov::LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult FovPlugin::OnPluginEnd() {
	// Symmetric teardown (PITFALLS A7). Each step is guarded: the vip core may already be going
	// away on a full unload, but the remaining registrations must still be dropped.
	if (m_clientLoadedRegistered && VipCoreReady() &&
		__vip_VIP_OnClientLoaded_Unregister != nullptr) {
		try {
			g_pVIPCore->VIP_OnClientLoadedUnregister();
		} catch (...) {
		}
	}
	m_clientLoadedRegistered = false;
	if (m_spawnRegistered && VipCoreReady() && __vip_VIP_OnPlayerSpawn_Unregister != nullptr) {
		try {
			g_pVIPCore->VIP_OnPlayerSpawnUnregister();
		} catch (...) {
		}
	}
	m_spawnRegistered = false;
	// The vip API has no feature unregister: re-registering the feature without callbacks replaces
	// the core's entry with null delegates, so the core keeps no pointer into this library after it
	// is unloaded (same value/feature type: the menu entry is unchanged). Only for a feature this
	// instance really registered.
	if (m_featureRegistered && VipCoreReady() && __vip_VIP_RegisterFeature != nullptr) {
		try {
			g_pVIPCore->VIP_RegisterFeature(vip_fov::kFeature, VIP_BOOL, SELECTABLE);
		} catch (...) {
		}
	}
	m_featureRegistered = false;
	// Destroy every menu and the reserved id 0 while the map gate still tells ShutdownMenus
	// whether a backend close is safe.
	try {
		vip_fov::ShutdownMenus();
	} catch (...) {
	}
	try {
		vip_fov::SetMapLifecycleCallbacks(nullptr, nullptr);
		vip_fov::ShutdownMapGate();
	} catch (...) {
	}
	// A reload resolves the schema field against the live map again (PITFALLS C8).
	vip_fov::ClearSchemaOffsets();
	for (std::vector<std::string>& list : m_fov) {
		list.clear();
	}
	for (int32_t iSlot = 0; iSlot < vip_fov::kMaxSlots; ++iSlot) {
		m_pendingLoad[iSlot] = 0;
		m_pendingSpawn[iSlot] = 0;
	}
	m_seeded = false;
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
