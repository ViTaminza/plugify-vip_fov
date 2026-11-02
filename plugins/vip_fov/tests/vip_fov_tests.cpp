// vip_fov_tests — the test binary of the pure-s2sdk vip_fov plugin.
//
// The plugin TU itself is compiled into the test (like vip_test's test), so the whole lifecycle —
// OnPluginStart / OnPluginUpdate / OnPluginEnd, the vip core callbacks, the map listeners and the
// native s2sdk menu handler — runs exactly as on a server, while every __s2sdk_* / __vip_* /
// __hudmenu_* binding slot is a recording fake. There is no pisexcompat anywhere in this build.
//
// Covered:
//   1. The pure logic (fov_logic.hpp, no binding touched): the original's std::getline list
//      semantics, the guarded std::stoi, the item<7 / item==7 dispatch, the Pisex page-to-button
//      mapping and the s2sdk menu-type choice.
//   2. The guards (PITFALLS B1/B2, C1/C2, C8): no client/schema call before the first map or in
//      the OnMapEnd window, the slot/controller identity chain, signed handle filtering (0 and -1
//      invalid, other negatives may be valid) and the map-gated, success-only-cached offset.
//   3. The spawn behaviour: default 90, the stored "FOV_Value" cookie, a malformed cookie, the
//      non-VIP/empty-list skips, and the exact write (handle, 4 bytes, changeState, chain -1).
//   4. The native menu flow: title/items/exit/back/pagination/close-on-select/timeout, the
//      selection (cookie + FOV + re-open), the back button (VIP menu), exit/timeout, the page
//      guard (digits 6/7) and the menu-type resolution through the optional hudmenu binding.
//   5. Owner identity: a Show or Select queued for player A is dropped when the slot is re-used by
//      player B (different SteamID and controller handle) or A loses VIP before the update.
//   6. A map change: menus are ended through CancelClientMenu(Disconnect) and destroyed, pending
//      work is dropped/retried, no engine call happens in the window, and the next map re-arms
//      everything. A DB-loaded callback in the OnMapEnd window is replayed on the next map.
//   7. Reuse: a second display replaces and destroys the previous menu exactly once, queued shows
//      collapse per slot, and Cancel+End do not double-destroy.
//   8. Bindings: the complete menu lifecycle set is required before ANY menu/reserved-id creation;
//      a missing create/add/display/get/cancel/destroy setter or GetClientSteamID64 creates
//      nothing. A partially present vip registration pair (Register/Unregister) is never called;
//      other vip getters/setters fail soft. No unsafe backend close at map end / unload; an
//      unconfirmed cancel falls back to the built-in no-op-close menu type, and a menu that even
//      that cannot remove is explicitly reported as A7_UNRESOLVED.
//   9. Teardown/reload (PITFALLS A1/A7/C8): every vip callback, map listener, menu (including the
//      reserved MenuId 0) and schema offset is dropped; a reload re-arms everything once and seeds
//      already-loaded VIPs without granting non-VIPs.
//
// Run by ../../test.sh (build/tests/vip_fov_tests), or directly from the build.

#include "../src/plugin.cpp"

#include <hudmenu/hudmenu.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include "common.hpp"
#include "fov_logic.hpp"
#include "guards.hpp"
#include "menus.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// --- checks -----------------------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), static_cast<long long>(expected),
					static_cast<long long>(actual));
	}
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected '%.*s', got '%.*s')\n", what.c_str(), static_cast<int>(expected.size()),
					expected.data(), static_cast<int>(actual.size()), actual.data());
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(std::string_view(text));
}

plg::string ToPlg(std::string_view text) {
	return plg::string(text);
}

std::string JoinList(const std::vector<std::string>& values) {
	std::string joined;
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i > 0) {
			joined += '|';
		}
		joined += values[i];
	}
	return joined;
}

// --- fake log ---------------------------------------------------------------------------------

std::vector<std::string> g_logLines;
plg::Severity g_lastSeverity = plg::Severity::Unknown;

bool LogContains(std::string_view needle) {
	for (const std::string& line : g_logLines) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
	g_lastSeverity = severity;
}

// --- fake s2sdk: map listeners ----------------------------------------------------------------

std::vector<s2sdk::OnMapStartCallback> g_mapStartCallbacks;
std::vector<s2sdk::OnMapEndCallback> g_mapEndCallbacks;
int g_mapStartRegisterCalls = 0;
int g_mapEndRegisterCalls = 0;
int g_mapStartUnregisterCalls = 0;
int g_mapEndUnregisterCalls = 0;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	++g_mapStartRegisterCalls;
	for (const auto& registered : g_mapStartCallbacks) {
		if (registered == callback) {
			return; // s2sdk keeps one entry per function
		}
	}
	g_mapStartCallbacks.push_back(callback);
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	++g_mapStartUnregisterCalls;
	for (auto it = g_mapStartCallbacks.begin(); it != g_mapStartCallbacks.end();) {
		it = *it == callback ? g_mapStartCallbacks.erase(it) : it + 1;
	}
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	++g_mapEndRegisterCalls;
	for (const auto& registered : g_mapEndCallbacks) {
		if (registered == callback) {
			return;
		}
	}
	g_mapEndCallbacks.push_back(callback);
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	++g_mapEndUnregisterCalls;
	for (auto it = g_mapEndCallbacks.begin(); it != g_mapEndCallbacks.end();) {
		it = *it == callback ? g_mapEndCallbacks.erase(it) : it + 1;
	}
}

void SimulateMapStart() {
	const std::vector<s2sdk::OnMapStartCallback> callbacks = g_mapStartCallbacks;
	for (const s2sdk::OnMapStartCallback callback : callbacks) {
		if (callback != nullptr) {
			callback();
		}
	}
}

void SimulateMapEnd() {
	const std::vector<s2sdk::OnMapEndCallback> callbacks = g_mapEndCallbacks;
	for (const s2sdk::OnMapEndCallback callback : callbacks) {
		if (callback != nullptr) {
			callback();
		}
	}
}

// --- fake s2sdk: clients ----------------------------------------------------------------------

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool hasController = true;
	int32_t controllerHandle = 100;
	uint64_t steamId = 0;
};

constexpr uint64_t kSteamIdBase = 76561190000000000ull;

std::array<FakeClient, vip_fov::kMaxSlots> g_clients;
bool g_entityPointerValid = true;
std::set<int32_t> g_invalidHandles;
int g_connectedCalls = 0;

FakeClient& Client(int32_t slot) {
	return g_clients[static_cast<std::size_t>(slot)];
}

void Login(int32_t slot) {
	FakeClient& client = Client(slot);
	client.connected = true;
	client.inGame = true;
	client.sourceTV = false;
	client.hasController = true;
	client.controllerHandle = 100 + slot;
	client.steamId = kSteamIdBase + static_cast<uint64_t>(slot);
}

// Simulates the old player leaving and the same slot being taken by another valid client (new
// SteamID and a new controller generation) before OnPluginUpdate.
void ReplacePlayer(int32_t slot, uint64_t steamId, int32_t controllerHandle) {
	FakeClient& client = Client(slot);
	client = FakeClient{};
	client.connected = true;
	client.inGame = true;
	client.controllerHandle = controllerHandle;
	client.steamId = steamId;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_connectedCalls;
	return slot >= 0 && slot < vip_fov::kMaxSlots && Client(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	return slot >= 0 && slot < vip_fov::kMaxSlots && Client(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	return slot >= 0 && slot < vip_fov::kMaxSlots && Client(slot).sourceTV;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	if (slot < 0 || slot >= vip_fov::kMaxSlots || !Client(slot).connected || !Client(slot).hasController) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x100000 + static_cast<uintptr_t>(slot)));
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	if (slot < 0 || slot >= vip_fov::kMaxSlots) {
		return -1;
	}
	return Client(slot).controllerHandle;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	if (slot < 0 || slot >= vip_fov::kMaxSlots || !Client(slot).connected) {
		return 0;
	}
	return Client(slot).steamId;
}

bool FakeIsValidEntHandle(int32_t handle) {
	if (handle == 0 || handle == -1) {
		return false;
	}
	return g_invalidHandles.find(handle) == g_invalidHandles.end();
}

bool FakeIsValidEntPointer(void* entity) {
	return entity != nullptr && g_entityPointerValid;
}

// --- fake s2sdk: schema -----------------------------------------------------------------------

int32_t g_schemaOffset = 0x2A0;
int32_t g_schemaFailures = 0;
int g_schemaLookupCalls = 0;
std::vector<std::pair<std::string, std::string>> g_schemaLookups;

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_schemaLookupCalls;
	g_schemaLookups.emplace_back(ToStd(className), ToStd(memberName));
	if (g_schemaFailures > 0) {
		--g_schemaFailures;
		return -1;
	}
	return g_schemaOffset;
}

struct FakeWrite {
	int32_t handle = 0;
	int32_t offset = 0;
	int64_t value = 0;
	int32_t size = 0;
	bool changeState = false;
	int32_t chainOffset = 0;
};

std::vector<FakeWrite> g_writes;

void FakeSetEntData(int32_t handle, int32_t offset, int64_t value, int32_t size, bool changeState,
					int32_t chainOffset) {
	g_writes.push_back(FakeWrite{handle, offset, value, size, changeState, chainOffset});
}

// --- fake s2sdk: menus ------------------------------------------------------------------------

struct FakeMenu {
	std::string title;
	std::string type;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<std::pair<std::string, std::string>> items; // info, display
	int32_t pagination = 0;
	bool exitButton = false;
	bool exitBackButton = false;
	bool closeOnSelect = true;
};

std::map<s2sdk::MenuId, FakeMenu> g_menus;
s2sdk::MenuId g_nextMenuId = 0;
std::array<s2sdk::MenuId, vip_fov::kMaxSlots> g_displayed{};
std::array<int32_t, vip_fov::kMaxSlots> g_offsets{};
std::vector<s2sdk::MenuId> g_createdMenus;
std::vector<s2sdk::MenuId> g_destroyedMenus;
std::vector<int32_t> g_displayTimes;
std::array<bool, vip_fov::kMaxSlots> g_lastCancelWasDisconnect{};
int g_cancelCalls = 0;
int g_backendCloseCalls = 0;
bool g_cancelFails = false;
bool g_setMenuTypeFails = false;
bool g_getMenuTypeLies = false;
int g_setMenuTypeCalls = 0;
std::string g_lastSetMenuType;
std::set<std::string> g_registeredTypes = {"chat", "console", "centerhtml", "button", "hud", "wasd"};

void EndClient(int32_t slot, s2sdk::MenuCancelReason reason) {
	const s2sdk::MenuId id = g_displayed[static_cast<std::size_t>(slot)];
	if (id == 0) {
		return;
	}
	g_displayed[static_cast<std::size_t>(slot)] = 0;
	g_lastCancelWasDisconnect[static_cast<std::size_t>(slot)] =
		reason == s2sdk::MenuCancelReason::Disconnect;
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return;
	}
	const s2sdk::MenuHandlerCallback handler = it->second.handler;
	if (handler != nullptr) {
		handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
		handler(id, s2sdk::MenuAction::End, slot, 0);
	}
}

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler,
							 const plg::string& menuType) {
	FakeMenu menu;
	menu.title = ToStd(title);
	menu.type = ToStd(menuType);
	menu.handler = handler;
	const s2sdk::MenuId id = g_nextMenuId++;
	g_menus[id] = std::move(menu);
	g_createdMenus.push_back(id);
	return id;
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return false;
	}
	// s2sdk's DestroyMenu cancels every client still viewing it first (MenuCancelReason::Destroyed),
	// and that path DOES ask the backend to close the display - the very client call that must not
	// happen while the map is torn down. MenuId 0 is the "no menu" sentinel for every client state,
	// so the reserved dummy menu has no viewer to cancel.
	if (id != 0) {
		// ChatMenu_Close/ConsoleMenu_Close in s2sdk are no-ops; every other backend (hud/wasd/
		// centerhtml/button) touches the client on close.
		const bool noopClose = it->second.type == "chat" || it->second.type == "console";
		for (int32_t slot = 0; slot < vip_fov::kMaxSlots; ++slot) {
			if (g_displayed[static_cast<std::size_t>(slot)] != id) {
				continue;
			}
			if (!noopClose) {
				++g_backendCloseCalls;
			}
			EndClient(slot, s2sdk::MenuCancelReason::Destroyed);
		}
	}
	g_menus.erase(id);
	g_destroyedMenus.push_back(id);
	return true;
}

bool FakeIsMenuTypeRegistered(const plg::string& name) {
	return g_registeredTypes.find(ToStd(name)) != g_registeredTypes.end();
}

bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t itemsPerPage) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return false;
	}
	it->second.pagination = itemsPerPage;
	return true;
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return false;
	}
	it->second.exitButton = enabled;
	return true;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return false;
	}
	it->second.exitBackButton = enabled;
	return true;
}

bool FakeSetMenuCloseOnSelect(s2sdk::MenuId id, bool enabled) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return false;
	}
	it->second.closeOnSelect = enabled;
	return true;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& typeName) {
	++g_setMenuTypeCalls;
	if (g_setMenuTypeFails) {
		return false;
	}
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return false;
	}
	it->second.type = ToStd(typeName);
	g_lastSetMenuType = it->second.type;
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId id) {
	if (g_getMenuTypeLies) {
		return ToPlg("hud");
	}
	const auto it = g_menus.find(id);
	return it == g_menus.end() ? plg::string() : ToPlg(it->second.type);
}

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle style) {
	(void)style;
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return -1;
	}
	it->second.items.emplace_back(ToStd(info), ToStd(display));
	return static_cast<int32_t>(it->second.items.size()) - 1;
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t time) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end() || slot < 0 || slot >= vip_fov::kMaxSlots) {
		return false;
	}
	if (g_displayed[static_cast<std::size_t>(slot)] != 0) {
		EndClient(slot, s2sdk::MenuCancelReason::Interrupted);
	}
	g_displayed[static_cast<std::size_t>(slot)] = id;
	g_offsets[static_cast<std::size_t>(slot)] = 0;
	g_displayTimes.push_back(time);
	if (it->second.handler != nullptr) {
		it->second.handler(id, s2sdk::MenuAction::Start, slot, 0);
	}
	return true;
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	++g_cancelCalls;
	if (g_cancelFails) {
		return false; // simulate a cancel that cannot confirm the session end
	}
	if (slot < 0 || slot >= vip_fov::kMaxSlots ||
		g_displayed[static_cast<std::size_t>(slot)] == 0) {
		return false;
	}
	if (reason != s2sdk::MenuCancelReason::Disconnect) {
		++g_backendCloseCalls; // s2sdk would notify the backend for any reason but Disconnect
	}
	EndClient(slot, reason);
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	if (slot < 0 || slot >= vip_fov::kMaxSlots) {
		return 0;
	}
	return g_displayed[static_cast<std::size_t>(slot)];
}

int32_t FakeGetClientMenuOffset(int32_t slot) {
	if (slot < 0 || slot >= vip_fov::kMaxSlots) {
		return 0;
	}
	return g_offsets[static_cast<std::size_t>(slot)];
}

// --- fake hudmenu -----------------------------------------------------------------------------

std::string g_playerMenuType = "hud";

plg::string FakeGetClientMenuType(int32_t /*slot*/) {
	return ToPlg(g_playerMenuType);
}

// --- fake vip core ----------------------------------------------------------------------------

struct FakeFeature {
	bool registered = false;
	std::string name;
	int valueType = 0;
	int featureType = 0;
	vip::ItemSelectableDelegate select = nullptr;
	vip::ItemTogglableDelegate toggle = nullptr;
	vip::ItemDisplayDelegate display = nullptr;
};

FakeFeature g_feature;
int g_featureRegistrations = 0;
int g_clientLoadedRegistrations = 0;
int g_clientLoadedUnregistrations = 0;
int g_spawnRegistrations = 0;
int g_spawnUnregistrations = 0;
vip::ClientLoadedOrDisconnectDelegate g_clientLoaded = nullptr;
vip::SpawnDelegate g_spawn = nullptr;
std::map<int32_t, std::string> g_featureValues;
std::map<int32_t, std::map<std::string, std::string>> g_cookies;
std::map<int32_t, bool> g_vipClients; // missing = VIP
struct CookieWrite {
	int32_t slot = 0;
	std::string name;
	std::string value;
};
std::vector<CookieWrite> g_cookieWrites;
std::vector<int32_t> g_openedVipMenus;
int g_featureStringCalls = 0;
std::map<int32_t, int> g_featureStringCallsBySlot;
int g_cookieGetCalls = 0;
int g_vipCheckCalls = 0;

void FakeRegisterClientLoaded(vip::ClientLoadedOrDisconnectDelegate callback) {
	++g_clientLoadedRegistrations;
	g_clientLoaded = callback;
}

void FakeUnregisterClientLoaded(vip::ClientLoadedOrDisconnectDelegate callback) {
	++g_clientLoadedUnregistrations;
	if (g_clientLoaded == callback) {
		g_clientLoaded = nullptr;
	}
}

void FakeRegisterSpawn(vip::SpawnDelegate callback) {
	++g_spawnRegistrations;
	g_spawn = callback;
}

void FakeUnregisterSpawn(vip::SpawnDelegate callback) {
	++g_spawnUnregistrations;
	if (g_spawn == callback) {
		g_spawn = nullptr;
	}
}

plg::string FakeGetTranslate(const plg::string& phrase) {
	if (ToStd(phrase) == vip_fov::kTitlePhrase) {
		return ToPlg("FOV Title");
	}
	return plg::string();
}

plg::string FakeGetClientFeatureString(int32_t slot, const plg::string& /*feature*/) {
	++g_featureStringCalls;
	++g_featureStringCallsBySlot[slot];
	const auto it = g_featureValues.find(slot);
	return it == g_featureValues.end() ? plg::string() : ToPlg(it->second);
}

plg::string FakeGetClientCookie(int32_t slot, const plg::string& name) {
	++g_cookieGetCalls;
	const auto slotIt = g_cookies.find(slot);
	if (slotIt == g_cookies.end()) {
		return plg::string();
	}
	const auto it = slotIt->second.find(ToStd(name));
	return it == slotIt->second.end() ? plg::string() : ToPlg(it->second);
}

bool FakeSetClientCookie(int32_t slot, const plg::string& name, const plg::string& data) {
	g_cookieWrites.push_back(CookieWrite{slot, ToStd(name), ToStd(data)});
	g_cookies[slot][ToStd(name)] = ToStd(data);
	return true;
}

bool FakeVipIsClientVIP(int32_t slot) {
	++g_vipCheckCalls;
	const auto it = g_vipClients.find(slot);
	return it == g_vipClients.end() ? true : it->second;
}

void FakeRegisterFeature(const plg::string& feature, vip::ValueType valueType, vip::FeatureType featureType,
						 vip::ItemSelectableDelegate select, vip::ItemTogglableDelegate toggle,
						 vip::ItemDisplayDelegate display) {
	++g_featureRegistrations;
	g_feature.registered = true;
	g_feature.name = ToStd(feature);
	g_feature.valueType = static_cast<int>(valueType);
	g_feature.featureType = static_cast<int>(featureType);
	g_feature.select = select;
	g_feature.toggle = toggle;
	g_feature.display = display;
}

void FakeOpenVipMenu(int32_t slot) {
	g_openedVipMenus.push_back(slot);
}

bool FakeVipIsVIPLoaded() {
	return true;
}

// --- fake installation ------------------------------------------------------------------------

constexpr int kRequiredMenuBindingCount = 13;

// 0 = OnMapStart_Register, 1 = OnMapStart_Unregister, 2 = OnMapEnd_Register,
// 3 = OnMapEnd_Unregister.
void DisableMapBinding(int which) {
	switch (which) {
	case 0: __s2sdk_OnMapStart_Register = nullptr; break;
	case 1: __s2sdk_OnMapStart_Unregister = nullptr; break;
	case 2: __s2sdk_OnMapEnd_Register = nullptr; break;
	case 3: __s2sdk_OnMapEnd_Unregister = nullptr; break;
	default: break;
	}
}

void DisableRequiredMenuBinding(int which) {
	switch (which) {
	case 0: __s2sdk_CreateMenu = nullptr; break;
	case 1: __s2sdk_AddMenuItem = nullptr; break;
	case 2: __s2sdk_DisplayMenu = nullptr; break;
	case 3: __s2sdk_GetClientMenu = nullptr; break;
	case 4: __s2sdk_CancelClientMenu = nullptr; break;
	case 5: __s2sdk_DestroyMenu = nullptr; break;
	case 6: __s2sdk_SetMenuPagination = nullptr; break;
	case 7: __s2sdk_SetMenuExitButton = nullptr; break;
	case 8: __s2sdk_SetMenuExitBackButton = nullptr; break;
	case 9: __s2sdk_SetMenuCloseOnSelect = nullptr; break;
	case 10: __s2sdk_GetClientSteamID64 = nullptr; break;
	case 11: __s2sdk_SetMenuType = nullptr; break;
	case 12: __s2sdk_GetMenuType = nullptr; break;
	default: break;
	}
}

enum class VipBindingSlot {
	ClientLoadedRegister,
	ClientLoadedUnregister,
	SpawnRegister,
	SpawnUnregister,
	RegisterFeature,
	GetClientFeatureString,
	GetClientCookie,
	SetClientCookie,
	IsClientVIP,
	GetTranslate,
	OpenMenu,
};

void DisableVipBinding(VipBindingSlot which) {
	switch (which) {
	case VipBindingSlot::ClientLoadedRegister: __vip_VIP_OnClientLoaded_Register = nullptr; break;
	case VipBindingSlot::ClientLoadedUnregister: __vip_VIP_OnClientLoaded_Unregister = nullptr; break;
	case VipBindingSlot::SpawnRegister: __vip_VIP_OnPlayerSpawn_Register = nullptr; break;
	case VipBindingSlot::SpawnUnregister: __vip_VIP_OnPlayerSpawn_Unregister = nullptr; break;
	case VipBindingSlot::RegisterFeature: __vip_VIP_RegisterFeature = nullptr; break;
	case VipBindingSlot::GetClientFeatureString: __vip_VIP_GetClientFeatureString = nullptr; break;
	case VipBindingSlot::GetClientCookie: __vip_VIP_GetClientCookie = nullptr; break;
	case VipBindingSlot::SetClientCookie: __vip_VIP_SetClientCookie = nullptr; break;
	case VipBindingSlot::IsClientVIP: __vip_VIP_IsClientVIP = nullptr; break;
	case VipBindingSlot::GetTranslate: __vip_VIP_GetTranslate = nullptr; break;
	case VipBindingSlot::OpenMenu: __vip_VIP_OpenMenu = nullptr; break;
	}
}

void UnbindAll() {
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientInGame = nullptr;
	__s2sdk_IsClientSourceTV = nullptr;
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	__s2sdk_GetClientSteamID64 = nullptr;
	__s2sdk_IsValidEntHandle = nullptr;
	__s2sdk_IsValidEntPointer = nullptr;
	__s2sdk_GetSchemaOffset = nullptr;
	__s2sdk_SetEntData = nullptr;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapStart_Unregister = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnMapEnd_Unregister = nullptr;
	__s2sdk_IsMenuTypeRegistered = nullptr;
	__s2sdk_CreateMenu = nullptr;
	__s2sdk_DestroyMenu = nullptr;
	__s2sdk_SetMenuPagination = nullptr;
	__s2sdk_SetMenuExitButton = nullptr;
	__s2sdk_SetMenuExitBackButton = nullptr;
	__s2sdk_SetMenuCloseOnSelect = nullptr;
	__s2sdk_AddMenuItem = nullptr;
	__s2sdk_DisplayMenu = nullptr;
	__s2sdk_CancelClientMenu = nullptr;
	__s2sdk_GetClientMenu = nullptr;
	__s2sdk_GetClientMenuOffset = nullptr;
	__s2sdk_SetMenuType = nullptr;
	__s2sdk_GetMenuType = nullptr;
	__hudmenu_GetClientMenuType = nullptr;
	__vip_VIP_IsVIPLoaded = nullptr;
	__vip_VIP_OnClientLoaded_Register = nullptr;
	__vip_VIP_OnClientLoaded_Unregister = nullptr;
	__vip_VIP_OnPlayerSpawn_Register = nullptr;
	__vip_VIP_OnPlayerSpawn_Unregister = nullptr;
	__vip_VIP_GetTranslate = nullptr;
	__vip_VIP_GetClientFeatureString = nullptr;
	__vip_VIP_GetClientCookie = nullptr;
	__vip_VIP_SetClientCookie = nullptr;
	__vip_VIP_IsClientVIP = nullptr;
	__vip_VIP_RegisterFeature = nullptr;
	__vip_VIP_OpenMenu = nullptr;
}

void BindS2sdk() {
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_SetEntData = &FakeSetEntData;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuCloseOnSelect = &FakeSetMenuCloseOnSelect;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_GetClientMenuOffset = &FakeGetClientMenuOffset;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
}

void BindVip() {
	__vip_VIP_IsVIPLoaded = &FakeVipIsVIPLoaded;
	__vip_VIP_OnClientLoaded_Register = &FakeRegisterClientLoaded;
	__vip_VIP_OnClientLoaded_Unregister = &FakeUnregisterClientLoaded;
	__vip_VIP_OnPlayerSpawn_Register = &FakeRegisterSpawn;
	__vip_VIP_OnPlayerSpawn_Unregister = &FakeUnregisterSpawn;
	__vip_VIP_GetTranslate = &FakeGetTranslate;
	__vip_VIP_GetClientFeatureString = &FakeGetClientFeatureString;
	__vip_VIP_GetClientCookie = &FakeGetClientCookie;
	__vip_VIP_SetClientCookie = &FakeSetClientCookie;
	__vip_VIP_IsClientVIP = &FakeVipIsClientVIP;
	__vip_VIP_RegisterFeature = &FakeRegisterFeature;
	__vip_VIP_OpenMenu = &FakeOpenVipMenu;
}

void ResetFakes() {
	g_logLines.clear();
	g_lastSeverity = plg::Severity::Unknown;
	g_mapStartCallbacks.clear();
	g_mapEndCallbacks.clear();
	g_mapStartRegisterCalls = 0;
	g_mapEndRegisterCalls = 0;
	g_mapStartUnregisterCalls = 0;
	g_mapEndUnregisterCalls = 0;
	g_clients = {};
	g_entityPointerValid = true;
	g_invalidHandles.clear();
	g_connectedCalls = 0;
	g_schemaOffset = 0x2A0;
	g_schemaFailures = 0;
	g_schemaLookupCalls = 0;
	g_schemaLookups.clear();
	g_writes.clear();
	g_menus.clear();
	g_nextMenuId = 0;
	g_displayed = {};
	g_offsets = {};
	g_createdMenus.clear();
	g_destroyedMenus.clear();
	g_displayTimes.clear();
	g_lastCancelWasDisconnect = {};
	g_cancelCalls = 0;
	g_backendCloseCalls = 0;
	g_cancelFails = false;
	g_setMenuTypeFails = false;
	g_getMenuTypeLies = false;
	g_setMenuTypeCalls = 0;
	g_lastSetMenuType.clear();
	g_registeredTypes = {"chat", "console", "centerhtml", "button", "hud", "wasd"};
	g_playerMenuType = "hud";
	g_feature = FakeFeature{};
	g_featureRegistrations = 0;
	g_clientLoadedRegistrations = 0;
	g_clientLoadedUnregistrations = 0;
	g_spawnRegistrations = 0;
	g_spawnUnregistrations = 0;
	g_clientLoaded = nullptr;
	g_spawn = nullptr;
	g_featureValues.clear();
	g_cookies.clear();
	g_vipClients.clear();
	g_cookieWrites.clear();
	g_openedVipMenus.clear();
	g_featureStringCalls = 0;
	g_featureStringCallsBySlot.clear();
	g_cookieGetCalls = 0;
	g_vipCheckCalls = 0;
	UnbindAll();
	BindS2sdk();
	BindVip();
	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;
	plg::Log = &FakeLog;
}

void RunUpdate() {
	const plg::PluginResult result = g_plugin.OnPluginUpdate(std::chrono::milliseconds(0));
	CheckEqInt(static_cast<int>(result.GetCode()), static_cast<int>(plg::PluginCode::Ok),
			   "OnPluginUpdate never fails");
}

void StartPlugin() {
	const plg::PluginResult result = g_plugin.OnPluginStart();
	CheckEqInt(static_cast<int>(result.GetCode()), static_cast<int>(plg::PluginCode::Ok),
			   "OnPluginStart never fails (PITFALLS A1)");
}

void EndPlugin() {
	const plg::PluginResult result = g_plugin.OnPluginEnd();
	CheckEqInt(static_cast<int>(result.GetCode()), static_cast<int>(plg::PluginCode::Ok),
			   "OnPluginEnd never fails (PITFALLS A7)");
}

const FakeMenu* MenuById(s2sdk::MenuId id) {
	const auto it = g_menus.find(id);
	return it == g_menus.end() ? nullptr : &it->second;
}

s2sdk::MenuId Displayed(int32_t slot) {
	if (slot < 0 || slot >= vip_fov::kMaxSlots) {
		return 0;
	}
	return g_displayed[static_cast<std::size_t>(slot)];
}

std::string JoinedItems(s2sdk::MenuId id) {
	const FakeMenu* menu = MenuById(id);
	if (menu == nullptr) {
		return "<missing>";
	}
	std::string joined;
	for (std::size_t i = 0; i < menu->items.size(); ++i) {
		if (i > 0) {
			joined += '|';
		}
		joined += menu->items[i].first;
	}
	return joined;
}

void SelectItem(int32_t slot, int32_t index) {
	const s2sdk::MenuId id = Displayed(slot);
	Check(id != 0, "a menu is displayed before selecting an item");
	const FakeMenu* menu = MenuById(id);
	if (menu == nullptr || menu->handler == nullptr) {
		Check(false, "the displayed menu has a handler");
		return;
	}
	menu->handler(id, s2sdk::MenuAction::Select, slot, index);
}

void CancelMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	const s2sdk::MenuId id = Displayed(slot);
	Check(id != 0, "a menu is displayed before cancelling it");
	const FakeMenu* menu = MenuById(id);
	if (menu == nullptr || menu->handler == nullptr) {
		Check(false, "the displayed menu has a handler");
		return;
	}
	const s2sdk::MenuHandlerCallback handler = menu->handler;
	// s2sdk's EndClientMenu() clears the client state first, then reports Cancel and End.
	g_displayed[static_cast<std::size_t>(slot)] = 0;
	g_lastCancelWasDisconnect[static_cast<std::size_t>(slot)] =
		reason == s2sdk::MenuCancelReason::Disconnect;
	handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
	handler(id, s2sdk::MenuAction::End, slot, 0);
}

void OpenFovMenuViaFeature(int32_t slot) {
	Check(g_feature.select != nullptr, "the FOV feature has its select callback");
	if (g_feature.select == nullptr) {
		return;
	}
	const bool closes = g_feature.select(slot, ToPlg(vip_fov::kFeature));
	Check(!closes, "the feature callback returns false (the original's return value)");
}

// ==============================================================================================
// 1. the pure logic
// ==============================================================================================

void TestPureLogic() {
	std::printf("== pure logic\n");

	CheckEqStr(JoinList(vip_fov::ParseFovList("")), "", "empty list");
	CheckEqStr(JoinList(vip_fov::ParseFovList("90")), "90", "single value");
	CheckEqStr(JoinList(vip_fov::ParseFovList("90,100")), "90|100", "two values");
	CheckEqStr(JoinList(vip_fov::ParseFovList("90,")), "90", "no trailing empty token");
	CheckEqStr(JoinList(vip_fov::ParseFovList(",90")), "|90", "leading empty token");
	CheckEqStr(JoinList(vip_fov::ParseFovList("90,,100")), "90||100", "middle empty token");
	CheckEqStr(JoinList(vip_fov::ParseFovList(" 90 , 100 ")), " 90 | 100 ", "spaces are kept");

	int value = 0;
	Check(vip_fov::TryParseFov("90", value) && value == 90, "stoi 90");
	Check(vip_fov::TryParseFov("-5", value) && value == -5, "stoi -5");
	Check(vip_fov::TryParseFov("90abc", value) && value == 90, "stoi stops at the first non-digit");
	Check(vip_fov::TryParseFov(" 90", value) && value == 90, "stoi skips leading spaces");
	Check(!vip_fov::TryParseFov("abc", value), "stoi rejects a non-number");
	Check(!vip_fov::TryParseFov("", value), "stoi rejects an empty string");
	Check(!vip_fov::TryParseFov("  ", value), "stoi rejects only spaces");
	Check(!vip_fov::TryParseFov("99999999999999999999", value), "stoi rejects out_of_range");

	CheckEqInt(static_cast<int>(vip_fov::ActionForItem(1)), static_cast<int>(vip_fov::SelectAction::SetFov),
			   "item 1 selects");
	CheckEqInt(static_cast<int>(vip_fov::ActionForItem(6)), static_cast<int>(vip_fov::SelectAction::SetFov),
			   "item 6 selects");
	CheckEqInt(static_cast<int>(vip_fov::ActionForItem(7)),
			   static_cast<int>(vip_fov::SelectAction::OpenVipMenu), "item 7 opens the VIP menu");
	CheckEqInt(static_cast<int>(vip_fov::ActionForItem(9)), static_cast<int>(vip_fov::SelectAction::Ignore),
			   "item 9 is ignored");
	CheckEqInt(static_cast<int>(vip_fov::ActionForItem(vip_fov::kExitItem)),
			   static_cast<int>(vip_fov::SelectAction::Ignore), "the Pisex exit item is ignored");
	CheckEqInt(static_cast<int>(vip_fov::ActionForItem(0)), static_cast<int>(vip_fov::SelectAction::SetFov),
			   "item 0 selects (below 7, like the original)");

	CheckEqInt(vip_fov::VisibleMenuButton(0, 0), 1, "page button of index 0");
	CheckEqInt(vip_fov::VisibleMenuButton(4, 0), 5, "page button of index 4");
	CheckEqInt(vip_fov::VisibleMenuButton(5, 0), 0, "index 5 is digit 6 of the next page");
	CheckEqInt(vip_fov::VisibleMenuButton(6, 0), 0, "index 6 is digit 7 of the next page");
	CheckEqInt(vip_fov::VisibleMenuButton(5, 5), 1, "index 5 on page 2 is button 1");
	CheckEqInt(vip_fov::VisibleMenuButton(9, 5), 5, "index 9 on page 2 is button 5");
	CheckEqInt(vip_fov::VisibleMenuButton(-1, 0), 0, "negative index");

	CheckEqStr(vip_fov::PickMenuTypeName("hud", true, true), "hud", "player choice hud");
	CheckEqStr(vip_fov::PickMenuTypeName("hud", false, true), "button", "hud unregistered -> button");
	CheckEqStr(vip_fov::PickMenuTypeName("wasd", true, true), "wasd", "player choice wasd");
	CheckEqStr(vip_fov::PickMenuTypeName("wasd", true, false), "button", "wasd unregistered -> button");
	CheckEqStr(vip_fov::PickMenuTypeName("chat", true, true), "chat", "player choice chat");
	CheckEqStr(vip_fov::PickMenuTypeName("centerhtml", true, true), "centerhtml",
			   "player choice centerhtml");
	CheckEqStr(vip_fov::PickMenuTypeName("button", true, true), "button", "player choice button");
	CheckEqStr(vip_fov::PickMenuTypeName("", true, true), "hud", "default HUD_LAYOUT with hud");
	CheckEqStr(vip_fov::PickMenuTypeName("", false, true), "button", "default HUD_LAYOUT without hud");
	CheckEqStr(vip_fov::PickMenuTypeName("garbage", true, true), "hud", "unknown choice falls back");
}

// ==============================================================================================
// 2. spawn behaviour and guards
// ==============================================================================================

void TestSpawnApply() {
	std::printf("== spawn apply / guards\n");
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100,110";
	StartPlugin();
	CheckEqInt(g_mapStartRegisterCalls, 1, "OnPluginStart registers OnMapStart once");
	CheckEqInt(g_mapEndRegisterCalls, 1, "OnPluginStart registers OnMapEnd once");
	CheckEqInt(g_clientLoadedRegistrations, 1, "OnPluginStart registers VIP_OnClientLoaded once");
	CheckEqInt(g_spawnRegistrations, 1, "OnPluginStart registers VIP_OnPlayerSpawn once");
	CheckEqInt(g_featureRegistrations, 1, "OnPluginStart registers the FOV feature once");
	Check(g_feature.select != nullptr, "the feature has a select callback");
	Check(g_feature.toggle == nullptr, "the feature has no toggle callback");
	CheckEqStr(g_feature.name, "FOV", "feature name");
	CheckEqInt(g_feature.valueType, static_cast<int>(vip::VIP_BOOL), "feature value type VIP_BOOL");
	CheckEqInt(g_feature.featureType, static_cast<int>(vip::SELECTABLE), "feature type SELECTABLE");

	// Before the first map nothing may reach a client, the schema or the vip core (PITFALLS B1).
	Check(!vip_fov::IsMapReady(), "the gate is shut right after OnPluginStart");
	Check(g_clientLoaded != nullptr && g_spawn != nullptr, "both VIP callbacks were registered");
	g_clientLoaded(1, true);
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 0, "no schema write before the first map");
	CheckEqInt(g_connectedCalls, 0, "no client query before the first map");
	CheckEqInt(g_schemaLookupCalls, 0, "no schema lookup before the first map (PITFALLS C8)");
	CheckEqInt(g_featureStringCalls, 0, "no vip FeatureString call before the first map");
	CheckEqInt(g_cookieGetCalls, 0, "no vip cookie call before the first map");

	// The first update opens the gate and replays both pending callbacks with a fresh identity.
	RunUpdate();
	Check(vip_fov::IsMapReady(), "the first update opens the gate for a mid-map load");
	CheckEqInt(static_cast<int>(g_writes.size()), 1, "the deferred spawn applies the default FOV");
	if (!g_writes.empty()) {
		CheckEqInt(g_writes[0].handle, 101, "the write uses the controller handle");
		CheckEqInt(g_writes[0].offset, 0x2A0, "the write uses the resolved schema offset");
		CheckEqInt(g_writes[0].value, 90, "an empty cookie defaults to 90");
		CheckEqInt(g_writes[0].size, 4, "m_iDesiredFOV is 4 bytes");
		Check(g_writes[0].changeState, "the write changes the network state");
		CheckEqInt(g_writes[0].chainOffset, -1, "an entity field passes chainOffset -1");
	}
	CheckEqInt(g_featureStringCalls, 1, "the deferred OnClientLoaded re-read the feature once");
	CheckEqInt(g_cookieGetCalls, 1, "the deferred spawn read the cookie once");
	CheckEqInt(g_schemaLookupCalls, 1, "the schema offset is resolved once");
	CheckEqInt(static_cast<int>(g_schemaLookups.size()), 1, "one schema lookup recorded");
	if (!g_schemaLookups.empty()) {
		CheckEqStr(g_schemaLookups[0].first, "CBasePlayerController", "schema class");
		CheckEqStr(g_schemaLookups[0].second, "m_iDesiredFOV", "schema field");
	}
	g_writes.clear(); // the deferred write is asserted; the rest of the scenario starts clean

	g_cookies[1][vip_fov::kCookieName] = "110";
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 1, "the stored cookie is applied");
	if (!g_writes.empty()) {
		CheckEqInt(g_writes[0].value, 110, "the stored cookie value is written");
	}
	CheckEqInt(g_schemaLookupCalls, 1, "a successful offset is cached (C8)");

	g_cookies[1][vip_fov::kCookieName] = "abc";
	g_logLines.clear();
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 1, "a malformed cookie leaves the FOV unchanged");
	Check(LogContains("non-numeric"), "a malformed cookie is logged");
	Check(g_lastSeverity == plg::Severity::Error, "the malformed cookie log is an error");

	g_cookies[1][vip_fov::kCookieName].clear();
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "an empty cookie falls back to 90 again");
	if (g_writes.size() >= 2) {
		CheckEqInt(g_writes[1].value, 90, "the fallback is 90");
	}

	// Non-VIP and an empty list never write.
	g_spawn(1, 2, false);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "a non-VIP spawn does not write");
	g_featureValues[1].clear();
	g_clientLoaded(1, true);
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "an empty FOV list does not write");
	g_featureValues[1] = "90,100,110";
	g_clientLoaded(1, true);

	// Slot identities.
	g_spawn(-1, 2, true);
	g_spawn(64, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "out-of-range slots are rejected");
	g_clientLoaded(-1, true);
	g_clientLoaded(64, true);

	// The C2 identity chain: every layer stops the write.
	Client(1).connected = false;
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "a disconnected client is rejected");
	Login(1);
	Client(1).inGame = false;
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "a client that is not in game is rejected");
	Login(1);
	Client(1).sourceTV = true;
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "SourceTV is rejected");
	Login(1);
	Client(1).hasController = false;
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "a missing controller pointer is rejected");
	Login(1);
	Client(1).controllerHandle = 0;
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "handle 0 is rejected");
	Client(1).controllerHandle = -1;
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "handle -1 is rejected");
	Client(1).controllerHandle = 123;
	g_invalidHandles.insert(123);
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 2, "a handle the entity system rejects is not written");
	g_invalidHandles.clear();
	// A valid handle may be negative once the serial counter passes 65535 (PITFALLS C1): the
	// numeric filter must not be "handle > 0".
	Client(1).controllerHandle = -123456;
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 3, "a valid negative handle is accepted");
	if (g_writes.size() >= 3) {
		CheckEqInt(g_writes[2].handle, -123456, "the negative controller handle is written");
	}
	Client(1).controllerHandle = 101;

	// Schema failures are retried instead of poisoning the field (C8).
	vip_fov::ClearSchemaOffsets();
	g_schemaFailures = 1;
	const int beforeFailure = g_schemaLookupCalls;
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 3, "a failed schema lookup writes nothing");
	CheckEqInt(g_schemaLookupCalls, beforeFailure + 1, "the failed lookup is retried, not cached");
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 4, "the retried lookup applies the FOV");
	CheckEqInt(g_schemaLookupCalls, beforeFailure + 2, "the retry is one more lookup");

	EndPlugin();
}

// ==============================================================================================
// 3. callbacks arriving in the OnMapEnd window
// ==============================================================================================

void TestOnClientLoadedMapEndWindow() {
	std::printf("== OnMapEnd DB-loaded callback\n");
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	g_spawn(1, 2, true);
	const int writesBefore = static_cast<int>(g_writes.size());
	const int featureCallsBefore = g_featureStringCalls;
	const int cookieCallsBefore = g_cookieGetCalls;
	CheckEqInt(writesBefore, 1, "the spawn on map 1 applied the FOV");

	// The vip core fires VIP_OnClientLoaded from its async DB completion even while the map is
	// being torn down; the plugin must not call FeatureString/IsSlotHuman in that window.
	SimulateMapEnd();
	g_featureValues[1] = "120,130";
	g_cookies[1][vip_fov::kCookieName] = "130";
	g_clientLoaded(1, true); // DB-loaded callback in the OnMapEnd window
	g_spawn(1, 2, true);
	CheckEqInt(g_featureStringCalls, featureCallsBefore,
			   "no vip FeatureString call in the OnMapEnd window");
	CheckEqInt(g_cookieGetCalls, cookieCallsBefore, "no vip cookie call in the OnMapEnd window");
	CheckEqInt(static_cast<int>(g_writes.size()), writesBefore, "no schema write in the OnMapEnd window");

	// The next map replays the pending load/spawn with the then-current identity and VIP status.
	SimulateMapStart();
	RunUpdate();
	CheckEqInt(g_featureStringCalls, featureCallsBefore + 1,
			   "the deferred OnClientLoaded re-read the feature on the next map");
	CheckEqInt(g_cookieGetCalls, cookieCallsBefore + 1, "the deferred spawn read the cookie on the next map");
	CheckEqInt(static_cast<int>(g_writes.size()), writesBefore + 1, "the deferred spawn applied the FOV");
	if (static_cast<int>(g_writes.size()) == writesBefore + 1) {
		CheckEqInt(g_writes.back().value, 130, "the new cookie value was applied");
	}

	EndPlugin();
}

// ==============================================================================================
// 4. the native menu flow
// ==============================================================================================

void TestMenuFlow() {
	std::printf("== menu flow\n");
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100,110";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);

	// Opening the menu from the VIP core's select callback only queues it (no re-entrant CreateMenu
	// while the vip menu's handler holds the menu manager's mutex).
	const int createdBefore = static_cast<int>(g_createdMenus.size());
	OpenFovMenuViaFeature(1);
	CheckEqInt(static_cast<int>(g_createdMenus.size()), createdBefore, "the select callback only queues");
	RunUpdate();

	const s2sdk::MenuId id = Displayed(1);
	Check(id != 0, "the FOV menu is displayed after the update");
	const FakeMenu* menu = MenuById(id);
	Check(menu != nullptr, "the displayed menu exists");
	if (menu != nullptr) {
		CheckEqStr(menu->title, "FOV Title", "the title comes from VIP_GetTranslate(FOV_Title)");
		CheckEqStr(JoinedItems(id), "90|100|110", "one item per FOV value");
		CheckEqStr(menu->items[0].second, "90", "the item display is the value");
		CheckEqInt(menu->pagination, 5, "five items per page");
		Check(menu->exitButton, "the exit button is enabled");
		Check(menu->exitBackButton, "the exit-back button is enabled");
		Check(!menu->closeOnSelect, "close-on-select is off (the callback re-opens the menu)");
		CheckEqStr(menu->type, "hud", "the hudmenu player choice is used");
	}
	CheckEqInt(static_cast<int>(g_displayTimes.size()), 1, "one display recorded");
	if (!g_displayTimes.empty()) {
		CheckEqInt(g_displayTimes[0], 60, "the cs2-menus 60 s timeout");
	}
	Check(vip_fov::IsMenuOpen(1), "the plugin tracks the open menu");

	// A selection is recorded by the handler under the menu lock and applied on the next update.
	SelectItem(1, 1); // value "100"
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), 0, "no cookie write under the menu lock");
	CheckEqInt(static_cast<int>(g_writes.size()), 0, "no schema write under the menu lock");
	RunUpdate();
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), 1, "the selection writes the cookie once");
	if (!g_cookieWrites.empty()) {
		CheckEqInt(g_cookieWrites[0].slot, 1, "the cookie is written for the selecting slot");
		CheckEqStr(g_cookieWrites[0].name, "FOV_Value", "the cookie name");
		CheckEqStr(g_cookieWrites[0].value, "100", "the selected value");
	}
	CheckEqInt(static_cast<int>(g_writes.size()), 1, "the selection applies the FOV");
	if (!g_writes.empty()) {
		CheckEqInt(g_writes[0].value, 100, "the selected FOV");
	}
	const s2sdk::MenuId reopened = Displayed(1);
	Check(reopened != 0 && reopened != id, "the menu is re-opened with a fresh id");
	Check(MenuById(id) == nullptr, "the old menu is destroyed (no s2sdk leak)");
	CheckEqStr(JoinedItems(reopened), "90|100|110", "the re-opened menu has the same values");

	// An out-of-list index is ignored instead of selecting an invisible item.
	const std::size_t writesBefore = g_writes.size();
	SelectItem(1, 7);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_writes.size()), static_cast<int>(writesBefore),
			   "an out-of-list index writes no FOV");
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), 1, "an out-of-list index writes no cookie");

	// The exit-back button (Pisex item 7) re-opens the VIP menu.
	const std::size_t vipOpenBefore = g_openedVipMenus.size();
	CancelMenu(1, s2sdk::MenuCancelReason::ExitBack);
	CheckEqInt(static_cast<int>(g_openedVipMenus.size()), static_cast<int>(vipOpenBefore),
			   "the back action is queued, not run under the lock");
	RunUpdate();
	CheckEqInt(static_cast<int>(g_openedVipMenus.size()), static_cast<int>(vipOpenBefore) + 1,
			   "the back button opens the VIP menu");
	if (!g_openedVipMenus.empty()) {
		CheckEqInt(g_openedVipMenus.back(), 1, "the VIP menu opens for the same slot");
	}
	CheckEqInt(Displayed(1), 0, "the FOV menu is closed after the back button");
	Check(MenuById(reopened) == nullptr, "the back-button menu is destroyed");

	// The exit button (Pisex item 9) is ignored by the original callback.
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId exitMenu = Displayed(1);
	Check(exitMenu != 0, "the menu is open before exit");
	const std::size_t vipOpenBeforeExit = g_openedVipMenus.size();
	CancelMenu(1, s2sdk::MenuCancelReason::Exit);
	RunUpdate();
	CheckEqInt(Displayed(1), 0, "the exit button closes the menu");
	CheckEqInt(static_cast<int>(g_openedVipMenus.size()), static_cast<int>(vipOpenBeforeExit),
			   "the exit button does not open the VIP menu");
	Check(MenuById(exitMenu) == nullptr, "the exited menu is destroyed");

	// A timeout just drops the menu.
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId timeoutMenu = Displayed(1);
	CancelMenu(1, s2sdk::MenuCancelReason::Timeout);
	RunUpdate();
	CheckEqInt(Displayed(1), 0, "the timed-out menu is closed");
	CheckEqInt(static_cast<int>(g_openedVipMenus.size()), static_cast<int>(vipOpenBeforeExit),
			   "a timeout does not open the VIP menu");
	Check(MenuById(timeoutMenu) == nullptr, "the timed-out menu is destroyed");

	// A non-numeric item does nothing (the original's std::stoi threw into the handler): no
	// cookie write, no FOV write and no re-open.
	g_featureValues[1] = "abc,100";
	g_clientLoaded(1, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const int cookiesBefore = static_cast<int>(g_cookieWrites.size());
	const int writesBeforeNonNumeric = static_cast<int>(g_writes.size());
	const s2sdk::MenuId nonNumericMenu = Displayed(1);
	SelectItem(1, 0); // "abc"
	RunUpdate();
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), cookiesBefore, "a non-numeric item writes no cookie");
	CheckEqInt(static_cast<int>(g_writes.size()), writesBeforeNonNumeric,
			   "a non-numeric item writes no FOV");
	CheckEqInt(Displayed(1), nonNumericMenu,
			   "a non-numeric item leaves the menu open without re-opening it");

	// A selection whose client disconnects before the update is dropped: no cookie, no FOV write.
	OpenFovMenuViaFeature(1);
	RunUpdate();
	SelectItem(1, 1);
	const int cookiesBeforeDisconnect = static_cast<int>(g_cookieWrites.size());
	const int writesBeforeDisconnect = static_cast<int>(g_writes.size());
	Client(1).connected = false;
	RunUpdate();
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), cookiesBeforeDisconnect,
			   "a disconnected selecting client writes no cookie");
	CheckEqInt(static_cast<int>(g_writes.size()), writesBeforeDisconnect,
			   "a disconnected selecting client writes no FOV");
	Login(1);

	EndPlugin();
}

// ==============================================================================================
// 5. pages and menu type resolution
// ==============================================================================================

void TestPagesAndTypes() {
	std::printf("== pages / menu types\n");
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100,110,120,130,140,150";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	Check(Displayed(1) != 0, "the seven-item menu is open");

	// Page 2: GetClientMenuOffset is 5, item index 5 is visible button 1 -> value 140.
	g_offsets[1] = 5;
	SelectItem(1, 5);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), 1, "page 2 selection writes the cookie");
	if (!g_cookieWrites.empty()) {
		CheckEqStr(g_cookieWrites[0].value, "140", "page 2 index 5 is the visible button 1");
	}

	// An out-of-list index on page 2 is ignored.
	const std::size_t writesBefore = g_writes.size();
	SelectItem(1, 9);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_writes.size()), static_cast<int>(writesBefore),
			   "an out-of-list index is ignored");

	// Page 1, index 5 (digit 6) is ignored even though item index 5 exists.
	g_offsets[1] = 0;
	SelectItem(1, 5);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_writes.size()), static_cast<int>(writesBefore),
			   "digit 6 on page 1 is ignored");

	// The menu type follows hudmenu, then falls back to the Pisex HUD_LAYOUT mapping.
	auto openAndGetType = [](std::string_view expected, const std::string& what) {
		OpenFovMenuViaFeature(1);
		RunUpdate();
		const FakeMenu* menu = MenuById(Displayed(1));
		Check(menu != nullptr, what + ": the menu is open");
		CheckEqStr(menu != nullptr ? menu->type : "", expected, what);
	};
	g_playerMenuType = "chat";
	openAndGetType("chat", "the player's chat choice");
	g_playerMenuType = "centerhtml";
	openAndGetType("centerhtml", "the player's centerhtml choice");
	g_playerMenuType = "wasd";
	openAndGetType("wasd", "the player's wasd choice");
	g_playerMenuType = "hud";
	g_registeredTypes.erase("hud");
	openAndGetType("button", "hud unregistered falls back to button");
	g_registeredTypes.insert("hud");
	g_playerMenuType = "wasd";
	g_registeredTypes.erase("wasd");
	openAndGetType("button", "wasd unregistered falls back to button");
	g_registeredTypes.insert("wasd");
	__hudmenu_GetClientMenuType = nullptr;
	g_playerMenuType = "chat";
	openAndGetType("hud", "no hudmenu binding uses the HUD_LAYOUT default");
	g_registeredTypes.erase("hud");
	openAndGetType("button", "no hudmenu and no hud backend -> button");
	__hudmenu_GetClientMenuType = &FakeGetClientMenuType;

	EndPlugin();
}

// ==============================================================================================
// 6. a map change
// ==============================================================================================

void TestMapChange() {
	std::printf("== map change\n");
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100,110";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId menuId = Displayed(1);
	Check(menuId != 0, "the menu is open before the map change");

	// A selection queued right before the map change must be dropped (no client to serve).
	SelectItem(1, 1);
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), 0, "the selection is still queued");
	SimulateMapEnd();
	Check(!vip_fov::IsMapReady(), "OnMapEnd closes the gate (B2)");
	CheckEqInt(Displayed(1), 0, "the menu display ended at map end");
	Check(MenuById(menuId) == nullptr, "the menu was destroyed at map end");
	CheckEqInt(static_cast<int>(g_destroyedMenus.size()), 1, "exactly one menu destroyed at map end");
	Check(g_lastCancelWasDisconnect[1], "the session ended with MenuCancelReason::Disconnect");
	CheckEqInt(g_cancelCalls, 1, "CancelClientMenu(Disconnect) is used, not a backend close");
	CheckEqInt(g_backendCloseCalls, 0, "no menu backend close at map end (C10/B2)");

	// The changelevel window: no client, menu or schema call may happen (B1/B2/C13).
	const int connectedBefore = g_connectedCalls;
	const int createdBefore = static_cast<int>(g_createdMenus.size());
	RunUpdate();
	CheckEqInt(g_connectedCalls, connectedBefore, "no client query in the OnMapEnd window");
	CheckEqInt(static_cast<int>(g_createdMenus.size()), createdBefore, "no menu in the OnMapEnd window");
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 0, "no schema write in the OnMapEnd window");
	OpenFovMenuViaFeature(1);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_createdMenus.size()), createdBefore,
			   "a queued show is dropped at map end");
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), 0, "a queued selection is dropped at map end");

	// The next map start re-arms the gate; the spawn callback that arrived in the window is replayed.
	SimulateMapStart();
	Check(vip_fov::IsMapReady(), "OnMapStart opens the gate");
	RunUpdate();
	CheckEqInt(static_cast<int>(g_writes.size()), 1, "the deferred spawn applies the FOV on the next map");
	OpenFovMenuViaFeature(1);
	RunUpdate();
	Check(Displayed(1) != 0, "the menu works on the next map");
	CheckEqInt(g_mapStartRegisterCalls, 1, "the map listener is not duplicated across the map change");

	EndPlugin();
	CheckEqInt(static_cast<int>(g_mapStartCallbacks.size()), 0, "the map listener is removed on unload");
}

// ==============================================================================================
// 7. owner identity: a re-used slot never receives the old request
// ==============================================================================================

void TestOwnerIdentity() {
	std::printf("== owner identity\n");
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100,110";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);

	// Show queued for player A, then the slot is taken by player B before the update: no menu.
	const int createdBefore = static_cast<int>(g_createdMenus.size());
	OpenFovMenuViaFeature(1);
	ReplacePlayer(1, kSteamIdBase + 501, 701);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_createdMenus.size()), createdBefore,
			   "a Show for a re-used slot creates no menu");
	CheckEqInt(Displayed(1), 0, "a Show for a re-used slot displays nothing");

	// An unverifiable owner (no SteamID64) never gets a menu.
	Login(1);
	Client(1).steamId = 0;
	OpenFovMenuViaFeature(1);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_createdMenus.size()), createdBefore,
			   "a Show without a verified SteamID64 creates no menu");
	CheckEqInt(Displayed(1), 0, "a Show without a verified SteamID64 displays nothing");

	// Same for an expired VIP: the queued Show is dropped before CreateMenu.
	Login(1);
	g_vipClients[1] = false;
	OpenFovMenuViaFeature(1);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_createdMenus.size()), createdBefore,
			   "a Show for a no-longer-VIP player creates no menu");
	g_vipClients[1] = true;

	// Select queued for player A, then player B takes the slot: no cookie, no FOV write.
	Login(1);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId menuForA = Displayed(1);
	Check(menuForA != 0, "the menu is open for player A");
	SelectItem(1, 1);
	const int cookiesBefore = static_cast<int>(g_cookieWrites.size());
	const int writesBefore = static_cast<int>(g_writes.size());
	ReplacePlayer(1, kSteamIdBase + 502, 702);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), cookiesBefore,
			   "a Select for a re-used slot writes no cookie to player B");
	CheckEqInt(static_cast<int>(g_writes.size()), writesBefore,
			   "a Select for a re-used slot writes no FOV to player B");

	// The same with an expired VIP and an otherwise matching owner.
	Login(1);
	CheckEqInt(Displayed(1), menuForA, "the fake still shows the old menu");
	SelectItem(1, 1);
	const int cookiesBeforeExpired = static_cast<int>(g_cookieWrites.size());
	const int writesBeforeExpired = static_cast<int>(g_writes.size());
	g_vipClients[1] = false;
	RunUpdate();
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), cookiesBeforeExpired,
			   "an expired VIP selection writes no cookie");
	CheckEqInt(static_cast<int>(g_writes.size()), writesBeforeExpired,
			   "an expired VIP selection writes no FOV");
	g_vipClients[1] = true;

	EndPlugin();
}

// ==============================================================================================
// 8. reuse / no leaks
// ==============================================================================================

void TestReuse() {
	std::printf("== reuse\n");
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100,110";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);

	// Two selects before the update collapse into one queued display for the slot.
	const int createdBefore = static_cast<int>(g_createdMenus.size());
	OpenFovMenuViaFeature(1);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_createdMenus.size()), createdBefore + 1,
			   "queued shows collapse per slot");
	const s2sdk::MenuId first = Displayed(1);
	Check(first != 0, "the first menu is open");

	// A new display replaces the old one and destroys it exactly once.
	const int destroyedBefore = static_cast<int>(g_destroyedMenus.size());
	OpenFovMenuViaFeature(1);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_destroyedMenus.size()), destroyedBefore + 1,
			   "the replaced menu is destroyed once");
	Check(MenuById(first) == nullptr, "the previous menu is gone");
	const s2sdk::MenuId second = Displayed(1);
	Check(second != 0 && second != first, "a fresh menu replaced it");
	Check(vip_fov::IsMenuOpen(1), "exactly one menu is tracked");

	// Cancel + End report the same session end: destroy once.
	CancelMenu(1, s2sdk::MenuCancelReason::Exit);
	const s2sdk::MenuId ended = second;
	const FakeMenu* endedMenu = MenuById(ended);
	if (endedMenu != nullptr && endedMenu->handler != nullptr) {
		endedMenu->handler(ended, s2sdk::MenuAction::End, 1, 0);
	}
	RunUpdate();
	int destroyedCount = 0;
	for (const s2sdk::MenuId id : g_destroyedMenus) {
		if (id == ended) {
			++destroyedCount;
		}
	}
	CheckEqInt(destroyedCount, 1, "Cancel+End destroys the menu once");
	CheckEqInt(Displayed(1), 0, "the ended menu is no longer displayed");

	EndPlugin();
}

// ==============================================================================================
// 9. binding gates: menu lifecycle and partial vip registration pairs
// ==============================================================================================

void TestMenuBindingsGate() {
	std::printf("== menu binding gate\n");
	for (int which = 0; which < kRequiredMenuBindingCount; ++which) {
		ResetFakes();
		Login(1);
		g_featureValues[1] = "90,100";
		DisableRequiredMenuBinding(which);
		StartPlugin();
		RunUpdate();
		g_clientLoaded(1, true);
		OpenFovMenuViaFeature(1);
		RunUpdate();
		CheckEqInt(static_cast<int>(g_createdMenus.size()), 0,
				   "a missing lifecycle binding creates no menu (incl. MenuId 0)");
		CheckEqInt(g_backendCloseCalls, 0, "no backend close without a created menu");
		Check(LogContains("menu bindings are incomplete"), "the missing menu binding is logged");
		EndPlugin();
		CheckEqInt(static_cast<int>(g_menus.size()), 0, "no menu is left behind by the gate");
	}
}

void TestPartialVipBindings() {
	std::printf("== partial vip bindings\n");
	const VipBindingSlot kPairs[] = {
		VipBindingSlot::ClientLoadedRegister,
		VipBindingSlot::ClientLoadedUnregister,
		VipBindingSlot::SpawnRegister,
		VipBindingSlot::SpawnUnregister,
		VipBindingSlot::RegisterFeature,
	};
	for (const VipBindingSlot which : kPairs) {
		ResetFakes();
		Login(1);
		g_featureValues[1] = "90,100";
		DisableVipBinding(which);
		StartPlugin(); // must not call a null Register/Unregister slot
		RunUpdate();
		if (g_spawn != nullptr) {
			g_spawn(1, 2, true);
		}
		EndPlugin(); // must not call a null Unregister slot or leave a registered callback
		Check(g_clientLoaded == nullptr, "no VIP_OnClientLoaded callback is left registered");
		Check(g_spawn == nullptr, "no VIP_OnPlayerSpawn callback is left registered");
		if (which == VipBindingSlot::ClientLoadedRegister ||
			which == VipBindingSlot::ClientLoadedUnregister) {
			CheckEqInt(g_clientLoadedRegistrations, 0, "an incomplete client-loaded pair is not registered");
			CheckEqInt(g_clientLoadedUnregistrations, 0, "nothing is unregistered for an unregistered pair");
			CheckEqInt(g_spawnRegistrations, 1, "the intact spawn pair is still registered");
		}
		if (which == VipBindingSlot::SpawnRegister || which == VipBindingSlot::SpawnUnregister) {
			CheckEqInt(g_spawnRegistrations, 0, "an incomplete spawn pair is not registered");
			CheckEqInt(g_spawnUnregistrations, 0, "nothing is unregistered for an unregistered pair");
			CheckEqInt(g_clientLoadedRegistrations, 1, "the intact client-loaded pair is still registered");
		}
		if (which == VipBindingSlot::RegisterFeature) {
			CheckEqInt(g_featureRegistrations, 0, "the feature is not registered without its binding");
		}
		Check(LogContains("vip binding"), "the missing vip binding is logged");
	}

	// Present registration pairs but missing getters/setters: every path fails soft, no null call.
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100";
	StartPlugin();
	RunUpdate();
	DisableVipBinding(VipBindingSlot::GetClientFeatureString);
	DisableVipBinding(VipBindingSlot::GetClientCookie);
	DisableVipBinding(VipBindingSlot::SetClientCookie);
	DisableVipBinding(VipBindingSlot::IsClientVIP);
	DisableVipBinding(VipBindingSlot::GetTranslate);
	DisableVipBinding(VipBindingSlot::OpenMenu);
	g_clientLoaded(1, true);
	g_spawn(1, 2, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_writes.size()), 0, "no FOV is applied without the cookie getter");
	CheckEqInt(static_cast<int>(g_createdMenus.size()), 1, "only the reserved MenuId 0 exists");
	EndPlugin();
	CheckEqInt(g_clientLoadedUnregistrations, 1, "the intact pair is still unregistered on unload");
	CheckEqInt(g_spawnUnregistrations, 1, "the intact spawn pair is still unregistered on unload");

	// A missing cookie setter only skips the persistence: the menu still works and the FOV is
	// applied. A missing VIP_OpenMenu makes the back action a safe no-op.
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	DisableVipBinding(VipBindingSlot::SetClientCookie);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	SelectItem(1, 1);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_cookieWrites.size()), 0, "a missing cookie setter writes no cookie");
	CheckEqInt(static_cast<int>(g_writes.size()), 1, "the FOV is still applied without the setter");
	DisableVipBinding(VipBindingSlot::OpenMenu);
	CancelMenu(1, s2sdk::MenuCancelReason::ExitBack);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_openedVipMenus.size()), 0, "a missing VIP_OpenMenu is a safe no-op");
	EndPlugin();
}

void TestMissingBindings() {
	std::printf("== missing whole plugins\n");

	// No s2sdk and no vip at all: every entry point is a harmless no-op.
	ResetFakes();
	UnbindAll();
	StartPlugin();
	RunUpdate();
	EndPlugin();
	CheckEqInt(static_cast<int>(g_createdMenus.size()), 0, "no menu without the s2sdk bindings");

	// Menu bindings missing but clients/schema/vip present: the feature stays selectable, the
	// plugin just logs and displays nothing.
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100";
	__s2sdk_CreateMenu = nullptr;
	__s2sdk_AddMenuItem = nullptr;
	__s2sdk_DisplayMenu = nullptr;
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	CheckEqInt(static_cast<int>(g_createdMenus.size()), 0, "no menu without CreateMenu");
	Check(LogContains("menu bindings are incomplete"), "the missing menu bindings are logged");
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 1, "the spawn path still works without the menu bindings");
	EndPlugin();

	// Map listeners missing: the gate can never open, so no client/schema call happens.
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90";
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	StartPlugin();
	RunUpdate();
	Check(!vip_fov::IsMapReady(), "without map listeners the gate stays shut");
	g_spawn(1, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), 0, "no schema write without the map gate");
	Check(LogContains("map listeners are unavailable"), "the missing map listeners are logged");
	EndPlugin();

	// No vip bindings: OnPluginStart logs and stays alive (no wrapper call happens).
	ResetFakes();
	UnbindAll();
	BindS2sdk();
	StartPlugin();
	RunUpdate();
	EndPlugin();
	Check(LogContains("vip core bindings are unavailable"), "the missing vip bindings are logged");
}

// ==============================================================================================
// 10. map listener Register/Unregister pair gate
// ==============================================================================================

void TestMapListenerPairGate() {
	std::printf("== map listener pair gate\n");
	for (int which = 0; which < 4; ++which) {
		ResetFakes();
		Login(1);
		g_featureValues[1] = "90,100";
		DisableMapBinding(which);
		StartPlugin();
		RunUpdate();
		g_clientLoaded(1, true);
		g_spawn(1, 2, true);
		OpenFovMenuViaFeature(1);
		RunUpdate();
		CheckEqInt(g_mapStartRegisterCalls, 0, "no OnMapStart registration without its pair");
		CheckEqInt(g_mapEndRegisterCalls, 0, "no OnMapEnd registration without its pair");
		Check(!vip_fov::IsMapReady(), "a partial map-listener API keeps the gate shut");
		CheckEqInt(g_connectedCalls, 0, "no client query without the registered map listeners");
		CheckEqInt(g_schemaLookupCalls, 0, "no schema lookup without the gate");
		CheckEqInt(static_cast<int>(g_writes.size()), 0, "no schema write without the gate");
		CheckEqInt(Displayed(1), 0, "no menu is displayed without the gate");
		Check(LogContains("map listeners are unavailable"), "the incomplete map-listener API is logged");
		EndPlugin();
		CheckEqInt(g_mapStartUnregisterCalls, 0, "nothing is unregistered for an unregistered pair");
		CheckEqInt(g_mapEndUnregisterCalls, 0, "nothing is unregistered for an unregistered pair");
		CheckEqInt(static_cast<int>(g_mapStartCallbacks.size()), 0, "no map-start callback dangles");
		CheckEqInt(static_cast<int>(g_mapEndCallbacks.size()), 0, "no map-end callback dangles");
		Check(!vip_fov::IsMapReady(), "the gate stays shut after the unload");
	}
}

// ==============================================================================================
// 11. safe teardown at map end / unload
// ==============================================================================================

void TestSafeTeardown() {
	std::printf("== safe teardown\n");

	// 1. The normal path: CancelClientMenu(Disconnect) is confirmed, then DestroyMenu. No backend
	// close at any point.
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId normal = Displayed(1);
	Check(normal != 0, "the menu is open before map end");
	SimulateMapEnd();
	CheckEqInt(g_backendCloseCalls, 0, "the confirmed path never closes the backend");
	Check(MenuById(normal) == nullptr, "the menu is destroyed after the confirmed Disconnect cancel");
	CheckEqInt(Displayed(1), 0, "the client state no longer displays the menu");
	EndPlugin();

	// 2. The cancel cannot be confirmed, but the builtin-type escape works: switch to "chat" (its
	// s2sdk close is a no-op), verify the type and destroy - no unsafe backend close, no callback
	// left in the manager.
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId switched = Displayed(1);
	Check(switched != 0, "the second menu is open");
	g_cancelFails = true;
	SimulateMapEnd();
	CheckEqInt(g_backendCloseCalls, 0, "the builtin-type escape does no backend close");
	Check(g_setMenuTypeCalls > 0, "a builtin menu type was requested for the unconfirmed menu");
	CheckEqStr(g_lastSetMenuType, "chat", "the menu was switched to the no-op chat backend");
	Check(MenuById(switched) == nullptr, "the menu is destroyed through the no-op backend");
	int escapedDestroyed = 0;
	for (const s2sdk::MenuId destroyed : g_destroyedMenus) {
		if (destroyed == switched) {
			++escapedDestroyed;
		}
	}
	CheckEqInt(escapedDestroyed, 1, "the escaped menu really left the manager (no live callback)");
	CheckEqInt(Displayed(1), 0, "the client state no longer displays the escaped menu");
	EndPlugin();

	// 3. The map-end escape is temporarily unavailable, the unload retries it: the menu is still
	// destroyed safely (the active->map-end->unload sequence the review asked about).
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId unloadFallback = Displayed(1);
	Check(unloadFallback != 0, "the third menu is open");
	g_cancelFails = true;
	g_setMenuTypeFails = true;
	SimulateMapEnd();
	Check(MenuById(unloadFallback) != nullptr, "the menu survives the map end while both teardown paths fail");
	CheckEqInt(g_backendCloseCalls, 0, "no backend close while both teardown paths fail");
	g_setMenuTypeFails = false; // the unload retry can switch the type now
	EndPlugin();
	Check(MenuById(unloadFallback) == nullptr, "the unload retry destroys the menu through the no-op backend");
	CheckEqInt(static_cast<int>(g_menus.size()), 0, "no live menu callback is left after the unload retry");
	CheckEqInt(Displayed(1), 0, "the unloaded menu has no live client state");
	CheckEqInt(g_backendCloseCalls, 0, "the unload retry does no unsafe backend close");
	Check(!LogContains("A7_UNRESOLVED"), "a successful fallback does not report A7_UNRESOLVED");

	// 4. Cancel and the builtin-type escape both fail: the menu must not be destroyed with an
	// unsafe backend close. The branch is explicitly A7_UNRESOLVED and must not be deployed.
	ResetFakes();
	Login(1);
	g_featureValues[1] = "90,100";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId unresolved = Displayed(1);
	Check(unresolved != 0, "the fourth menu is open");
	g_cancelFails = true;
	g_setMenuTypeFails = true;
	SimulateMapEnd();
	EndPlugin();
	CheckEqInt(g_backendCloseCalls, 0, "an unresolved menu is never destroyed with an unsafe backend close");
	Check(MenuById(unresolved) != nullptr, "an unresolved menu is left in the manager, not destroyed unsafely");
	Check(LogContains("A7_UNRESOLVED"), "the unresolved teardown is explicitly reported as A7_UNRESOLVED");
}

// ==============================================================================================
// 12. teardown and reload
// ==============================================================================================

void TestTeardownAndReload() {
	std::printf("== teardown / reload\n");
	ResetFakes();
	Login(1);
	Login(2);
	g_vipClients[2] = false; // a non-VIP in slot 2 must never be seeded
	g_featureValues[1] = "90,100,110";
	g_featureValues[2] = "80,90";
	StartPlugin();
	RunUpdate();
	g_clientLoaded(1, true);
	g_clientLoaded(2, false);
	OpenFovMenuViaFeature(1);
	RunUpdate();
	const s2sdk::MenuId openId = Displayed(1);
	Check(openId != 0, "the menu is open before the unload");

	EndPlugin();
	CheckEqInt(g_clientLoadedUnregistrations, 1, "VIP_OnClientLoaded is unregistered");
	CheckEqInt(g_spawnUnregistrations, 1, "VIP_OnPlayerSpawn is unregistered");
	CheckEqInt(g_featureRegistrations, 2, "the feature is re-registered without callbacks");
	Check(g_feature.select == nullptr, "the core keeps no select callback into the unloaded library");
	CheckEqInt(static_cast<int>(g_mapStartCallbacks.size()), 0, "the map-start listener is removed");
	CheckEqInt(static_cast<int>(g_mapEndCallbacks.size()), 0, "the map-end listener is removed");
	CheckEqInt(static_cast<int>(g_menus.size()), 0, "every menu (including MenuId 0) is destroyed");
	Check(MenuById(openId) == nullptr, "the open menu is gone after the unload");
	Check(!vip_fov::IsMapReady(), "the gate is shut after the unload");
	Check(!vip_fov::IsMenuOpen(1), "no menu is tracked after the unload");

	// A reload (the same process, the same map) arms everything exactly once, seeds the still
	// connected VIP (slot 1) without reading the non-VIP (slot 2), and re-resolves the schema (A7/C8).
	const int featureCallsBefore = g_featureStringCalls;
	const int slot1Before = g_featureStringCallsBySlot[1];
	const int slot2Before = g_featureStringCallsBySlot[2];
	const int schemaCallsBefore = g_schemaLookupCalls;
	StartPlugin();
	CheckEqInt(g_mapStartRegisterCalls, 2, "the reload registers the map listeners again");
	CheckEqInt(g_clientLoadedRegistrations, 2, "the reload registers VIP_OnClientLoaded again");
	CheckEqInt(g_spawnRegistrations, 2, "the reload registers VIP_OnPlayerSpawn again");
	CheckEqInt(g_featureRegistrations, 3, "the reload registers the FOV feature again");
	Check(g_feature.select != nullptr, "the reload installs the select callback again");
	Check(!vip_fov::IsMapReady(), "the reload starts with the gate shut until the first update");
	RunUpdate();
	Check(vip_fov::IsMapReady(), "the first update of the reload opens the gate");
	CheckEqInt(g_featureStringCallsBySlot[1], slot1Before + 1, "the reload seeds the loaded VIP");
	CheckEqInt(g_featureStringCallsBySlot[2], slot2Before, "the reload does not read a non-VIP");
	CheckEqInt(g_featureStringCalls, featureCallsBefore + 1, "only one seed read happened");
	const std::size_t writesBefore = g_writes.size();
	g_spawn(2, 2, true);
	CheckEqInt(static_cast<int>(g_writes.size()), static_cast<int>(writesBefore),
			   "a non-VIP spawn is not given the FOV of the seeded VIP");
	g_clientLoaded(1, true);
	g_spawn(1, 2, true);
	Check(g_schemaLookupCalls > schemaCallsBefore, "the reload resolves the schema offset again");
	CheckEqInt(static_cast<int>(g_writes.size()), static_cast<int>(writesBefore) + 1,
			   "the reload applies the FOV again");
	OpenFovMenuViaFeature(1);
	RunUpdate();
	Check(Displayed(1) != 0, "the reload displays the menu again");
	EndPlugin();
}

} // namespace

int main() {
	std::printf("vip_fov tests\n");
	TestPureLogic();
	TestSpawnApply();
	TestOnClientLoadedMapEndWindow();
	TestMenuFlow();
	TestPagesAndTypes();
	TestMapChange();
	TestOwnerIdentity();
	TestReuse();
	TestMenuBindingsGate();
	TestPartialVipBindings();
	TestMissingBindings();
	TestMapListenerPairGate();
	TestSafeTeardown();
	TestTeardownAndReload();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
