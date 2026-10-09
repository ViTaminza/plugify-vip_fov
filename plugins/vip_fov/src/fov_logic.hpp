// vip_fov — engine-free part of the FOV module: the "FOV" list parsing and the menu/item
// dispatch of the original (ref/src/cs2-vip-modules/VIP_FOV/source/vip_fov.cpp).
//
// Kept pure so both are unit-testable (tests/vip_fov_tests.cpp).

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vip_fov {

constexpr int kMaxSlots = 64;

// The cookie the chosen FOV is stored in (the original's VIP_SetClientCookie name).
inline constexpr const char* kCookieName = "FOV_Value";
// The original defaulted to 90 when the cookie was empty.
inline constexpr int kDefaultFov = 90;
// The item number of the exit-back button in the Pisex menu numbering (pisexcompat's trampoline).
inline constexpr int kExitBackItem = 7;

// OnClientLoaded of the original parses the "FOV" group value with
// `std::istringstream iss(szFOV); while (std::getline(iss, token, ','))`:
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

inline SelectAction ActionForItem(int iItem) {
	if (iItem < kExitBackItem) {
		return SelectAction::SetFov;
	}
	if (iItem == kExitBackItem) {
		return SelectAction::OpenVipMenu;
	}
	return SelectAction::Ignore;
}

} // namespace vip_fov
