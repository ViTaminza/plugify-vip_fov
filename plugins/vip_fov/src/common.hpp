#pragma once
// vip_fov — constants and logging shared by the plugin, its guards, its menu controller and the
// test.
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7): there is no pisexcompat and no guard
// library. The FOV feature itself has no config file of its own — its value list comes from the
// vip core's groups.ini ("FOV") and its title from vip.phrases.txt ("FOV_Title"), both shipped by
// the vip package — so the only paths here are the schema names.

#include <plg/plugin.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace vip_fov {

// The original's GetLogTag() = "VIP-FOV"; log lines keep the "[VIP-FOV] " prefix.
inline constexpr char kLogTag[] = "VIP-FOV";

// The feature the original registered: VIP_BOOL, SELECTABLE ("FOV" in groups.ini).
inline constexpr char kFeature[] = "FOV";

// The menu title phrase (vip.phrases.txt, resolved through the vip core's VIP_GetTranslate).
inline constexpr char kTitlePhrase[] = "FOV_Title";

// The cookie the chosen FOV is stored in (the original's VIP_SetClientCookie name).
inline constexpr char kCookieName[] = "FOV_Value";

// CS2 has 64 player slots (PITFALLS C2: 0 <= slot < 64).
inline constexpr int32_t kMaxSlots = 64;

// The original defaulted to 90 when the cookie was empty.
inline constexpr int kDefaultFov = 90;

// cs2-menus shows five items per chat page; pisexcompat kept that pagination and so does the port.
inline constexpr int kItemsPerPage = 5;

// cs2-menus core.ini "MenuTime": pisexcompat's default and therefore the production timeout.
inline constexpr int kMenuTimeoutSeconds = 60;

// The Pisex menu item numbers the original's callback used: items below 7 select a FOV, item 7 is
// the exit-back button (which re-opens the VIP menu), item 9 is the exit button (ignored).
inline constexpr int kExitBackItem = 7;
inline constexpr int kExitItem = 9;

// The desired-FOV field of the original: CBasePlayerController::m_iDesiredFOV, an unsigned int
// (4 bytes, ref/s2sdk-src/src/core/sdk/entity/cbaseplayercontroller.h: SCHEMA_FIELD(uint, ...)).
inline constexpr char kFovClass[] = "CBasePlayerController";
inline constexpr char kFovField[] = "m_iDesiredFOV";
inline constexpr int32_t kFovFieldSize = static_cast<int32_t>(sizeof(uint32_t));

// 0 <= slot < 64 (PITFALLS C2). Nothing below a failed range check may reach an s2sdk export.
constexpr bool IsSlotInRange(int32_t iSlot) {
	return iSlot >= 0 && iSlot < kMaxSlots;
}

inline void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

inline void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// The original's ErrorLog("[%s] ...", GetLogTag()): the message keeps the "[VIP-FOV] " prefix.
inline void LogTaggedError(std::string_view message) {
	std::string line;
	line.reserve(sizeof(kLogTag) + message.size() + 4);
	line += '[';
	line += kLogTag;
	line += "] ";
	line.append(message.data(), message.size());
	LogError(line);
}

} // namespace vip_fov
