// vip_fov — the native s2sdk menu layer of the FOV module.
//
// The original built a Pisex menu (cs2-menus) and pisexcompat translated it onto s2sdk's menu
// manager. This file talks to the s2sdk menu API directly, keeping the visible behaviour:
//
//   * the title is the vip phrase "FOV_Title", one selectable item per value of the player's
//     "FOV" list (info == display == the value, exactly like the original's AddItemMenu),
//   * five items per page (cs2-menus pagination), an exit button and an exit-back button (the
//     original set both; s2sdk's digit backends show "Back" when the back button is enabled),
//   * close-on-select is OFF: the original callback kept the menu open and re-opened it itself
//     after storing the choice,
//   * selecting page item 1..5 -> the chosen value; the back button -> the VIP menu
//     (VIP_OpenMenu), the same routing the original's item<7 / item==7 callback had,
//   * the menu type follows the player's own "!menu" choice through the optional hudmenu plugin
//     (GetClientMenuType) and falls back to pisexcompat's HUD_LAYOUT mapping ("hud", or the
//     built-in "button" when the Panorama backend is not registered),
//   * a 60 s timeout (cs2-menus core.ini "MenuTime", pisexcompat's default).
//
// Lifecycle / safety rules that s2sdk leaves to the plugin:
//   * every menu/action request carries the verified OwnerIdentity captured when it was created
//     (SteamID64 + controller handle/generation). ApplyShow/ApplyAction re-capture and compare it
//     (and ask the plugin for the VIP entitlement) before CreateMenu, DisplayMenu, a cookie, a
//     schema write or VIP_OpenMenu; a request whose owner changed is dropped, never applied to a
//     player that re-used the slot,
//   * the complete menu binding set (create/add/display/get/cancel/destroy, the display setters
//     and GetClientSteamID64) must be present BEFORE the first registration/create, including the
//     reserved MenuId 0. Without it no menu is created at all, so no handler can be left behind,
//   * s2sdk NEVER destroys a menu by itself: every ended display (select/replace, exit, back,
//     timeout, disconnect) is queued and destroyed in ProcessMenuRequests() on a later
//     OnPluginUpdate, outside the menu manager's recursive mutex. The handler itself only records
//     an action - no client, schema, cookie or menu mutation runs under that lock,
//   * s2sdk uses MenuId 0 for "no menu" but assigns it to the first menu of the process, so
//     StartMenus() burns it with a dummy menu (pisexcompat did the same) and ShutdownMenus()
//     destroys it before the library unloads,
//   * at map end / unload while the map is down a displayed session is first ended with
//     CancelClientMenu(Disconnect) - s2sdk skips the menu backend for that reason. DestroyMenu is
//     called only after the client state no longer references the id. If the cancel cannot be
//     confirmed, the menu is switched to the built-in "chat"/"console" backend (SetMenuType +
//     GetMenuType verification; ChatMenu_Close/ConsoleMenu_Close are no-ops) and destroyed through
//     it, so no client call happens and no handler pointer stays in the manager (PITFALLS B2/C10,
//     A7). A menu that even that escape cannot remove is reported as A7_UNRESOLVED at unload and is
//     never destroyed with an unsafe backend close,
//   * every entry point re-checks the map gate, the slot range and the menu id it was handed, so a
//     stale engine callback after a reload cannot reach the plugin.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"
#include "guards.hpp"

namespace vip_fov {

// One planned display of the FOV menu. The menu type is resolved when the menu is really created
// (the player's hudmenu choice is asked at that moment, not while the VIP menu handler runs).
struct MenuPlan {
	std::string title;
	std::vector<std::string> values;
	bool exitButton = true;
	bool exitBackButton = true;
	int itemsPerPage = kItemsPerPage;
	int timeoutSeconds = kMenuTimeoutSeconds;
};

// The module callbacks. Plain function pointers, because the plugin passes one global instance and
// the menu manager itself only accepts plain function pointers (no captures). Every callback gets
// the verified owner of the request; the plugin re-checks it and the VIP entitlement before it
// touches the cookie/schema.
using MenuSelectHandler = void (*)(void* user, const OwnerIdentity& owner, const std::string& value);
using MenuExitBackHandler = void (*)(void* user, const OwnerIdentity& owner);
// "The owner is still an entitled VIP" - checked before CreateMenu/DisplayMenu and before a
// queued action reaches the plugin.
using MenuOwnerEligibleHandler = bool (*)(void* user, const OwnerIdentity& owner);

// Resets the menu state and burns MenuId 0. Only acts when the full menu binding set is present;
// touches only the menu manager (no client, no map, no schema).
void StartMenus();

// Installs the module callbacks (the eligible handler may be null, the others should not be).
void SetMenuHandlers(void* user, MenuSelectHandler select, MenuExitBackHandler exitBack,
					 MenuOwnerEligibleHandler eligible);

// Queues a display for the next ProcessMenuRequests(); never creates a menu inside the caller, so
// the feature-select callback (which runs inside the vip core's menu handler) cannot re-enter the
// menu manager. Dropped when the owner identity is not/no longer verified.
void DisplayMenuDeferred(const OwnerIdentity& owner, MenuPlan plan);

// Applies queued menu actions and destroys ended menus; called from OnPluginUpdate.
void ProcessMenuRequests();

// Drops pending work and destroys every menu of this plugin when the map goes down. Displayed
// sessions are ended through CancelClientMenu(Disconnect) first; a menu whose cancel cannot be
// confirmed stays queued for a later (safe) attempt.
void CloseMenusOnMapEnd();

// Destroys every menu, drops the pending queues and releases the reserved id 0; OnPluginEnd.
void ShutdownMenus();

// The s2sdk menu type this slot's menus are rendered with (see the header comment).
std::string ResolveMenuTypeName(int32_t iSlot);

// True when this plugin still has a menu displayed to the slot.
bool IsMenuOpen(int32_t iSlot);

// True when the s2sdk binding set the whole menu lifecycle depends on is present. Exposed for the
// test; StartMenus/ApplyShow use it as the gate before any registration/creation.
bool MenuBindingsAvailable();

} // namespace vip_fov
