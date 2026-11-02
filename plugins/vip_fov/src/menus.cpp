// vip_fov — the native s2sdk menu layer (see menus.hpp for the behaviour and safety rules it
// preserves).

#include "menus.hpp"

#include <hudmenu/hudmenu.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fov_logic.hpp"
#include "guards.hpp"

namespace vip_fov {
namespace {

struct OpenMenu {
	s2sdk::MenuId id = 0;
	OwnerIdentity owner;
	std::vector<std::string> values; // index-aligned with the s2sdk item list
};

struct PendingAction {
	enum class Kind {
		Select,
		ExitBack,
	};
	Kind kind = Kind::Select;
	OwnerIdentity owner;
	std::string value;
};

struct PendingShow {
	OwnerIdentity owner;
	MenuPlan plan;
};

std::array<OpenMenu, kMaxSlots> g_open;
std::vector<PendingAction> g_actions;
std::vector<PendingShow> g_shows;
std::vector<s2sdk::MenuId> g_destroy;

// True while a menu handler is running: the engine calls handlers with the menu manager's
// recursive mutex held, so menu/client/schema mutations are queued for ProcessMenuRequests().
bool g_inHandler = false;
// This plugin created the reserved s2sdk menu id 0 (see StartMenus) and must destroy it before
// unloading, or its handler points into unloaded code after `plg reload`.
bool g_reservedIdZero = false;
// One error line per library instance when the lifecycle bindings are incomplete.
bool g_bindingsErrorLogged = false;

void* g_user = nullptr;
MenuSelectHandler g_select = nullptr;
MenuExitBackHandler g_exitBack = nullptr;
MenuOwnerEligibleHandler g_eligible = nullptr;

void ReportMissingBindings() {
	if (g_bindingsErrorLogged) {
		return;
	}
	g_bindingsErrorLogged = true;
	LogTaggedError("the s2sdk menu bindings are incomplete (create/add/display/get/cancel/destroy, "
				   "the display setters or GetClientSteamID64 are missing) - the FOV menu is disabled");
}

// The "hud" backend (plugins/hudmenu) is cs2-menus' HUD_LAYOUT: a Panorama on-screen menu driven by
// mouse clicks, usable while dead.
bool IsTypeRegistered(std::string_view name) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string(name));
	} catch (...) {
		return false;
	}
}

// The player's own "!menu" choice, asked from the hudmenu plugin. Empty when hudmenu is not
// installed, the slot has no live client or anything went wrong - ResolveMenuTypeName then falls
// back to the Pisex default mapping.
std::string PlayerTypeChoice(int32_t iSlot) {
	if (__hudmenu_GetClientMenuType == nullptr) {
		return {};
	}
	try {
		const plg::string answer = hudmenu::GetClientMenuType(iSlot);
		return std::string(std::string_view(answer));
	} catch (...) {
		return {};
	}
}

void ClearSlot(int32_t iSlot) {
	g_open[static_cast<std::size_t>(iSlot)] = OpenMenu{};
}

bool IsTrackedMenu(s2sdk::MenuId id) {
	for (const OpenMenu& state : g_open) {
		if (state.id == id) {
			return true;
		}
	}
	return false;
}

// The owner is still the verified player of the request AND (when the plugin installed the hook)
// still an entitled VIP. Both checks run again right before anything is created/applied.
bool OwnerEligible(const OwnerIdentity& owner) {
	if (!OwnerMatches(owner)) {
		return false;
	}
	if (g_eligible == nullptr) {
		return true;
	}
	try {
		return g_eligible(g_user, owner);
	} catch (...) {
		return false;
	}
}

void QueueMenuDestroy(s2sdk::MenuId id) {
	if (id == 0) {
		return; // s2sdk's "no menu"
	}
	for (const s2sdk::MenuId queued : g_destroy) {
		if (queued == id) {
			return; // Cancel and End both report the same session end
		}
	}
	g_destroy.push_back(id);
}

// Ends every display of `id` with MenuCancelReason::Disconnect (s2sdk skips the menu backend for
// that reason, so no client call happens). Returns true only when no client state references the
// id any more - the precondition for a safe DestroyMenu(). A failed/refused cancel returns false.
bool EnsureNotDisplayed(s2sdk::MenuId id) {
	if (__s2sdk_GetClientMenu == nullptr || __s2sdk_CancelClientMenu == nullptr) {
		return false;
	}
	for (int32_t iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		s2sdk::MenuId current = 0;
		try {
			current = s2sdk::GetClientMenu(iSlot);
		} catch (...) {
			return false;
		}
		if (current != id) {
			continue;
		}
		try {
			s2sdk::CancelClientMenu(iSlot, s2sdk::MenuCancelReason::Disconnect);
		} catch (...) {
		}
		// Confirm the session is really gone before DestroyMenu may close the backend: this covers
		// both a refused cancel and an engine that reports success without clearing the state.
		try {
			if (s2sdk::GetClientMenu(iSlot) == id) {
				return false;
			}
		} catch (...) {
			return false;
		}
	}
	return true;
}

// The last-resort map-down teardown. A displayed session that refuses CancelClientMenu(Disconnect)
// can still be removed safely: switch the menu to one of the built-in digit backends whose close
// callback is a no-op (ChatMenu_Close / ConsoleMenu_Close in s2sdk's menu_types_builtin.cpp),
// verify the switch with GetMenuType, then DestroyMenu. DestroyMenu's Destroyed path then calls the
// no-op close instead of the custom hudmenu/wasd backend, so no client call happens and the menu
// (and its handler pointer) leaves the manager before the library unloads.
//   * SetMenuType is a plain menu-manager write and GetMenuType a read - both safe with the map down;
//   * false means the menu is gone already (nothing left to unload) or neither builtin type could be
//     verified; the caller keeps the id queued and, at unload, reports A7_UNRESOLVED.
bool ForceSafeDestroy(s2sdk::MenuId id) {
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr ||
		__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	constexpr const char* kSafeTypes[] = {"chat", "console"};
	for (const char* type : kSafeTypes) {
		bool switched = false;
		try {
			switched = s2sdk::SetMenuType(id, plg::string(type));
		} catch (...) {
			switched = false;
		}
		if (!switched) {
			// The menu no longer exists: there is no callback left to unload.
			try {
				if (s2sdk::GetMenuType(id).empty()) {
					return true;
				}
			} catch (...) {
			}
			continue;
		}
		plg::string actual;
		try {
			actual = s2sdk::GetMenuType(id);
		} catch (...) {
			continue;
		}
		if (std::string_view(actual) != type) {
			continue;
		}
		try {
			s2sdk::DestroyMenu(id); // Destroyed -> the builtin no-op close, no client call
			return true;
		} catch (...) {
			return false;
		}
	}
	return false;
}

// Runs outside the menu manager's lock. `allowBackendClose` is true only while the map is up (the
// unload-mid-map path), where DestroyMenu may safely close the backend display. With the gate
// closed the id goes through the confirmed-cancel path first and the builtin-type escape second.
void DestroyQueuedMenus(bool allowBackendClose) {
	if (g_destroy.empty()) {
		return;
	}
	std::vector<s2sdk::MenuId> ids = std::move(g_destroy);
	g_destroy.clear();
	for (const s2sdk::MenuId id : ids) {
		if (IsTrackedMenu(id)) {
			g_destroy.push_back(id); // a current display: ApplyShow/Shutdown owns it
			continue;
		}
		if (__s2sdk_DestroyMenu == nullptr) {
			g_destroy.push_back(id);
			continue;
		}
		const bool safe = id == 0 || EnsureNotDisplayed(id);
		if (!safe) {
			if (allowBackendClose && IsMapReady()) {
				try {
					s2sdk::DestroyMenu(id); // map is up: the backend close is safe
				} catch (...) {
				}
			} else if (!ForceSafeDestroy(id)) {
				g_destroy.push_back(id); // retried on the next update / at map start
			}
			continue;
		}
		try {
			s2sdk::DestroyMenu(id);
		} catch (...) {
			g_destroy.push_back(id);
		}
	}
}

// Must be called outside a handler and only while the map is up (DestroyMenu may close the backend
// display). Clears the slot first: DestroyMenu re-enters the menu manager and calls the handler's
// Cancel/End, which then sees a stale id and does nothing.
void DestroySlotMenu(int32_t iSlot) {
	const OpenMenu state = g_open[static_cast<std::size_t>(iSlot)];
	ClearSlot(iSlot);
	if (state.id != 0 && __s2sdk_DestroyMenu != nullptr) {
		try {
			s2sdk::DestroyMenu(state.id);
		} catch (...) {
		}
	}
}

void MenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t iSlot, int32_t iParam) {
	// Everything the handler asks for goes through the queues (see g_inHandler). The flag is
	// saved/restored rather than cleared, so a re-entering handler does not clear it for the outer
	// one.
	struct HandlerScope {
		bool previous;
		HandlerScope() : previous(g_inHandler) { g_inHandler = true; }
		~HandlerScope() { g_inHandler = previous; }
	} scope;
	try {
		if (!IsSlotInRange(iSlot)) {
			return;
		}
		OpenMenu& state = g_open[static_cast<std::size_t>(iSlot)];
		if (state.id != id) {
			return; // stale: the plugin already closed/replaced it
		}
		switch (action) {
		case s2sdk::MenuAction::Select: {
			if (iParam < 0 || static_cast<std::size_t>(iParam) >= state.values.size()) {
				return;
			}
			// s2sdk accepts digits 1..7 even when the page shows five items and maps them to
			// currentOffset + digit - 1, so 6/7 select an item of the next page that was never
			// displayed. Pisex numbered the five visible items 1..5 (pisexcompat's translation).
			int iOffset = iParam - iParam % kItemsPerPage;
			if (__s2sdk_GetClientMenuOffset != nullptr) {
				try {
					iOffset = s2sdk::GetClientMenuOffset(iSlot); // read-only, recursive mutex
				} catch (...) {
				}
			}
			if (VisibleMenuButton(iParam, iOffset) == 0) {
				return; // digit 6/7 (next page) or a stale index of another page
			}
			g_actions.push_back(PendingAction{PendingAction::Kind::Select, state.owner,
											 state.values[static_cast<std::size_t>(iParam)]});
			break;
		}
		case s2sdk::MenuAction::Cancel: {
			const s2sdk::MenuId menuId = state.id;
			const OwnerIdentity owner = state.owner;
			ClearSlot(iSlot);
			QueueMenuDestroy(menuId);
			// Pisex's exit/back values: the exit button (item 9) was ignored by the original, the
			// exit-back button (item 7) re-opened the VIP menu; a timeout/disconnect drops it.
			if (static_cast<s2sdk::MenuCancelReason>(iParam) == s2sdk::MenuCancelReason::ExitBack) {
				g_actions.push_back(PendingAction{PendingAction::Kind::ExitBack, owner, {}});
			}
			break;
		}
		case s2sdk::MenuAction::End: {
			const s2sdk::MenuId menuId = state.id;
			ClearSlot(iSlot);
			QueueMenuDestroy(menuId);
			break;
		}
		case s2sdk::MenuAction::Start:
			break;
		}
	} catch (...) {
		// Never unwind into the engine; the manager's lock must be released cleanly.
	}
}

void ApplyAction(const PendingAction& action) {
	if (!IsMapReady() || !OwnerEligible(action.owner)) {
		return; // stale owner / no longer a VIP: never apply to the current slot occupant
	}
	try {
		if (action.kind == PendingAction::Kind::Select) {
			if (g_select != nullptr) {
				g_select(g_user, action.owner, action.value);
			}
		} else if (g_exitBack != nullptr) {
			g_exitBack(g_user, action.owner);
		}
	} catch (...) {
		LogTaggedError("a queued menu action threw");
	}
}

void ApplyShow(PendingShow& show) {
	if (!IsMapReady()) {
		return;
	}
	if (!MenuBindingsAvailable()) {
		ReportMissingBindings();
		return;
	}
	if (!OwnerEligible(show.owner)) {
		return; // the request's player left / the slot was re-used / VIP expired: drop it
	}
	DestroySlotMenu(show.owner.slot); // replace whatever this client had open

	s2sdk::MenuId id = 0;
	try {
		id = s2sdk::CreateMenu(plg::string(show.plan.title), &MenuHandler,
							   plg::string(ResolveMenuTypeName(show.owner.slot)));
	} catch (...) {
		id = 0;
	}
	if (id == 0) {
		// Only possible when the id-0 reservation did not happen. The menu exists but cannot be
		// tracked (0 is "no menu"): destroy it while the map is up.
		if (__s2sdk_DestroyMenu != nullptr) {
			try {
				s2sdk::DestroyMenu(id);
			} catch (...) {
			}
		}
		LogTaggedError("s2sdk returned MenuId 0 - the FOV menu was not displayed");
		return;
	}

	OpenMenu& state = g_open[static_cast<std::size_t>(show.owner.slot)];
	state.id = id;
	state.owner = show.owner;
	for (const std::string& value : show.plan.values) {
		int32_t index = -1;
		try {
			index = s2sdk::AddMenuItem(id, plg::string(value), plg::string(value),
									   s2sdk::MenuItemStyle::Default);
		} catch (...) {
			index = -1;
		}
		if (index >= 0) {
			state.values.push_back(value); // index-aligned with the s2sdk item list
		}
	}
	// The complete binding set is a precondition (MenuBindingsAvailable above), so these cannot be
	// null calls; a throwing export still leaves a tracked menu that ShutdownMenus destroys.
	try {
		s2sdk::SetMenuPagination(id, show.plan.itemsPerPage);
		s2sdk::SetMenuExitButton(id, show.plan.exitButton);
		s2sdk::SetMenuExitBackButton(id, show.plan.exitBackButton);
		// The original kept the menu open after a selection and re-opened it itself.
		s2sdk::SetMenuCloseOnSelect(id, false);
	} catch (...) {
	}

	bool displayed = false;
	try {
		displayed = s2sdk::DisplayMenu(id, show.owner.slot, show.plan.timeoutSeconds);
	} catch (...) {
		displayed = false;
	}
	if (!displayed) {
		LogTaggedError("s2sdk could not display the FOV menu");
		DestroySlotMenu(show.owner.slot);
	}
}

void ReleaseReservedMenuId() {
	if (!g_reservedIdZero) {
		return;
	}
	g_reservedIdZero = false;
	if (__s2sdk_DestroyMenu == nullptr) {
		return;
	}
	try {
		s2sdk::DestroyMenu(0); // never displayed (0 is "no menu"), so no backend close
	} catch (...) {
	}
}

// The map-end / unload-while-map-down path: no client call may happen. Every tracked menu is
// queued and destroyed only after CancelClientMenu(Disconnect) has been confirmed; a refused
// cancel uses the builtin no-op-close type escape (ForceSafeDestroy) or leaves the id queued for a
// later safe attempt instead of forcing a backend close.
void CloseAllNoMap() {
	g_actions.clear();
	g_shows.clear();
	for (int32_t iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		const s2sdk::MenuId id = g_open[static_cast<std::size_t>(iSlot)].id;
		if (id == 0) {
			continue;
		}
		ClearSlot(iSlot);
		QueueMenuDestroy(id);
	}
	DestroyQueuedMenus(false);
}

} // namespace

// --- API --------------------------------------------------------------------------------------

bool MenuBindingsAvailable() {
	// The whole lifecycle and the owner identity, before ANY menu is registered/created:
	//   create/add/display  - the menu itself,
	//   get/cancel/destroy  - the only safe teardown path (s2sdk never destroys a menu itself),
	//   set/get menu type   - the map-down escape to a builtin backend with a no-op close,
	//   the display setters - the Pisex pagination/exit/back/close-on-select behaviour,
	//   GetClientSteamID64  - the owner identity every queued request is re-validated against.
	return __s2sdk_CreateMenu != nullptr && __s2sdk_AddMenuItem != nullptr &&
		   __s2sdk_DisplayMenu != nullptr && __s2sdk_GetClientMenu != nullptr &&
		   __s2sdk_CancelClientMenu != nullptr && __s2sdk_DestroyMenu != nullptr &&
		   __s2sdk_SetMenuPagination != nullptr && __s2sdk_SetMenuExitButton != nullptr &&
		   __s2sdk_SetMenuExitBackButton != nullptr && __s2sdk_SetMenuCloseOnSelect != nullptr &&
		   __s2sdk_SetMenuType != nullptr && __s2sdk_GetMenuType != nullptr &&
		   __s2sdk_GetClientSteamID64 != nullptr;
}

void StartMenus() {
	g_actions.clear();
	g_shows.clear();
	g_destroy.clear();
	for (OpenMenu& state : g_open) {
		state = OpenMenu{};
	}
	g_inHandler = false;
	g_bindingsErrorLogged = false; // one line per plugin instance
	g_user = nullptr;
	g_select = nullptr;
	g_exitBack = nullptr;
	g_eligible = nullptr;

	if (!MenuBindingsAvailable()) {
		// No reserved id 0 either: without the teardown bindings no menu of this plugin may exist.
		ReportMissingBindings();
		return;
	}

	// s2sdk's MenuManager starts ids at 0 but also uses 0 for "no menu", so the first menu created
	// in the process could never be displayed/tracked. Create and keep one dummy menu if we got id
	// 0 (cheap; done once per process by whichever plugin runs first).
	if (g_reservedIdZero) {
		return;
	}
	try {
		const s2sdk::MenuId id = s2sdk::CreateMenu(plg::string("vip_fov:reserved-id-0"), &MenuHandler,
												   plg::string("chat"));
		if (id != 0) {
			// id 0 was already taken elsewhere: nothing to keep.
			s2sdk::DestroyMenu(id);
		} else {
			g_reservedIdZero = true;
		}
	} catch (...) {
	}
}

void SetMenuHandlers(void* user, MenuSelectHandler select, MenuExitBackHandler exitBack,
					 MenuOwnerEligibleHandler eligible) {
	g_user = user;
	g_select = select;
	g_exitBack = exitBack;
	g_eligible = eligible;
}

void DisplayMenuDeferred(const OwnerIdentity& owner, MenuPlan plan) {
	if (!owner.IsSet() || !IsMapReady()) {
		return; // no verified owner / no map: nothing to show
	}
	if (!MenuBindingsAvailable()) {
		ReportMissingBindings();
		return;
	}
	if (!OwnerMatches(owner)) {
		return; // the player left or the slot was re-used between menu build and queueing
	}
	// Reuse the slot's queued show instead of piling up replacements (a fast second selection).
	for (PendingShow& queued : g_shows) {
		if (queued.owner.slot == owner.slot) {
			queued.owner = owner;
			queued.plan = std::move(plan);
			return;
		}
	}
	g_shows.push_back(PendingShow{owner, std::move(plan)});
}

void ProcessMenuRequests() {
	if (g_inHandler) {
		return; // never called from a handler; the queue is drained from OnPluginUpdate
	}
	if (!IsMapReady()) {
		// The changelevel window: no client, menu display or schema call. Ended menus are still
		// retried through the safe Disconnect/destroy path so they do not stay in the manager.
		g_actions.clear();
		g_shows.clear();
		DestroyQueuedMenus(false);
		return;
	}

	// Actions first: a selection stores the cookie, applies the FOV and re-opens the menu (which
	// only queues a new show). Then ended menus are freed, then queued shows are created - all
	// outside the menu manager's lock.
	std::vector<PendingAction> actions = std::move(g_actions);
	g_actions.clear();
	for (const PendingAction& action : actions) {
		ApplyAction(action);
	}
	DestroyQueuedMenus(false);

	std::vector<PendingShow> shows = std::move(g_shows);
	g_shows.clear();
	for (PendingShow& show : shows) {
		ApplyShow(show);
	}
	DestroyQueuedMenus(false);
}

void CloseMenusOnMapEnd() {
	CloseAllNoMap();
}

void ShutdownMenus() {
	g_actions.clear();
	g_shows.clear();
	if (IsMapReady() && MenuBindingsAvailable()) {
		// Mid-map unload (plg reload): DestroyMenu closes the backend display, which is safe while
		// the map and the client list exist - and it hides the HUD menu properly.
		for (int32_t iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
			if (g_open[static_cast<std::size_t>(iSlot)].id != 0) {
				DestroySlotMenu(iSlot);
			}
		}
		DestroyQueuedMenus(true);
	} else {
		// Map down (or bindings incomplete): cancel-confirmed first, then the builtin no-op-close
		// type escape; a menu neither path can remove is reported as A7_UNRESOLVED.
		CloseAllNoMap();
		if (!g_destroy.empty()) {
			DestroyQueuedMenus(false);
			if (!g_destroy.empty()) {
				LogTaggedError("A7_UNRESOLVED: could not unload a menu safely (cancel unconfirmed and "
							   "the builtin-type fallback failed) - it stays queued instead of forcing an "
							   "unsafe backend close");
			}
		}
	}
	ReleaseReservedMenuId();
	SetMenuHandlers(nullptr, nullptr, nullptr, nullptr);
}

std::string ResolveMenuTypeName(int32_t iSlot) {
	const bool hudRegistered = IsTypeRegistered("hud");
	const bool wasdRegistered = IsTypeRegistered("wasd");
	const std::string playerChoice = IsMapReady() ? PlayerTypeChoice(iSlot) : std::string();
	return PickMenuTypeName(playerChoice, hudRegistered, wasdRegistered);
}

bool IsMenuOpen(int32_t iSlot) {
	if (!IsSlotInRange(iSlot)) {
		return false;
	}
	const s2sdk::MenuId id = g_open[static_cast<std::size_t>(iSlot)].id;
	if (id == 0 || !IsMapReady() || __s2sdk_GetClientMenu == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetClientMenu(iSlot) == id;
	} catch (...) {
		return false;
	}
}

} // namespace vip_fov
