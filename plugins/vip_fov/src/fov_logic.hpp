// vip_fov — engine-free part of the FOV module: the "FOV" list parsing, the guarded std::stoi,
// the menu item dispatch of the original (vip_fov.cpp) and the two decisions the native menu
// controller needs (the Pisex page-to-button mapping and the s2sdk menu type choice).
//
// Kept pure so all of it is unit-testable (tests/vip_fov_tests.cpp).

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace vip_fov {

// OnClientLoaded of the original parses the "FOV" group value with std::getline over commas
// (std::istringstream semantics):
//   * an empty value yields no tokens,
//   * a trailing comma does not produce a final empty token,
//   * an empty element between two commas does ("90,,100" -> "90", "", "100"), and the menu then
//     shows an empty line whose selection the original's std::stoi would refuse.
inline std::vector<std::string> ParseFovList(std::string_view value) {
	std::vector<std::string> list;
	std::size_t start = 0;
	while (start < value.size()) {
		const std::size_t comma = value.find(',', start);
		if (comma == std::string_view::npos) {
			list.emplace_back(value.substr(start));
			break;
		}
		list.emplace_back(value.substr(start, comma - start));
		start = comma + 1;
	}
	return list;
}

// std::stoi with the original's parsing (leading whitespace ok, stops at the first non-digit),
// but without letting std::invalid_argument/out_of_range escape - the original's stoi threw into
// the spawn handler / menu callback on a malformed value.
inline bool TryParseFov(std::string_view text, int& out) {
	if (text.empty()) {
		return false;
	}
	try {
		std::size_t consumed = 0;
		out = std::stoi(std::string(text), &consumed);
		return consumed > 0;
	} catch (...) {
		return false;
	}
}

// The original's callback dispatch: items below 7 select a FOV, item 7 is the exit-back button
// (which re-opens the VIP menu); anything else (the exit button, a page turn) is ignored.
enum class SelectAction {
	Ignore,
	SetFov,
	OpenVipMenu,
};

constexpr SelectAction ActionForItem(int iItem) {
	if (iItem < kExitBackItem) {
		return SelectAction::SetFov;
	}
	if (iItem == kExitBackItem) {
		return SelectAction::OpenVipMenu;
	}
	return SelectAction::Ignore;
}

// The Pisex button number of a selected s2sdk item: pisexcompat translated s2sdk's global item
// index into the 1..5 number of the visible page, because s2sdk also accepts digits 6/7 and maps
// them to currentOffset + 5/6 - items of the next page that were never displayed. The original
// callback only ever saw 1..5, 7 and 9. Returns 0 for a selection outside the visible page.
// `pageOffset` is s2sdk::GetClientMenuOffset(slot); when the binding is unavailable the caller
// passes the page-aligned fallback `itemIndex - itemIndex % kItemsPerPage` (pisexcompat's).
constexpr int VisibleMenuButton(int iItemIndex, int iPageOffset) {
	if (iItemIndex < 0) {
		return 0;
	}
	const int iButton = iItemIndex - iPageOffset + 1;
	return (iButton >= 1 && iButton <= kItemsPerPage) ? iButton : 0;
}

// The s2sdk menu type a displayed menu gets, the pure part of pisexcompat's
// ResolveS2sdkMenuTypeName(): vip_fov never forces a type, so the player's own "!menu" choice
// (hudmenu's GetClientMenuType) wins when it is one of the known names; "hud"/"wasd" are only
// accepted while the matching backend is registered. Without a usable choice the Pisex default
// MenuType::HUD_LAYOUT is mapped exactly as pisexcompat mapped it: "hud" when the Panorama backend
// is registered, s2sdk's built-in "button" (the closest on-screen, dead-player-usable type)
// otherwise.
inline std::string PickMenuTypeName(std::string_view playerChoice, bool hudTypeRegistered,
									bool wasdTypeRegistered) {
	if (playerChoice == "chat" || playerChoice == "centerhtml" || playerChoice == "button") {
		return std::string(playerChoice);
	}
	if (playerChoice == "hud") {
		return hudTypeRegistered ? "hud" : "button";
	}
	if (playerChoice == "wasd") {
		return wasdTypeRegistered ? "wasd" : "button";
	}
	return hudTypeRegistered ? "hud" : "button";
}

} // namespace vip_fov
