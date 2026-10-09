// vip_fov unit tests — the engine-free part: the "FOV" list parsing (std::getline semantics),
// the guarded std::stoi and the menu item dispatch.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "fov_logic.hpp"

#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckEq(const std::string& actual, const std::string& expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected '%s', got '%s')\n", what.c_str(), expected.c_str(), actual.c_str());
	}
}

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

std::string Join(const std::vector<std::string>& values) {
	std::string joined;
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i > 0) {
			joined += '|';
		}
		joined += values[i];
	}
	return joined;
}

void CheckList(const char* input, const char* expected) {
	CheckEq(Join(vip_fov::ParseFovList(input)), expected, std::string("list '") + input + "'");
}

void CheckFov(const char* input, bool expected, int expectedValue) {
	int value = 0;
	const bool ok = vip_fov::TryParseFov(input, value);
	CheckEq(ok ? 1 : 0, expected ? 1 : 0, std::string("parse '") + input + "'");
	if (ok && expected) {
		CheckEq(value, expectedValue, std::string("value of '") + input + "'");
	}
}

} // namespace

int main() {
	std::printf("vip_fov tests\n");

	// The list parsing of the original's std::getline loop.
	CheckList("", "");
	CheckList("90", "90");
	CheckList("90,100", "90|100");
	CheckList("90,", "90");            // no trailing empty element
	CheckList(",90", "|90");           // leading empty element
	CheckList("90,,100", "90||100");   // an empty element in the middle
	CheckList(" 90 , 100 ", " 90 | 100 "); // getline keeps the spaces

	// The guarded stoi.
	CheckFov("90", true, 90);
	CheckFov("100", true, 100);
	CheckFov("-5", true, -5);
	CheckFov("90abc", true, 90); // stoi stops at the first non-digit
	CheckFov(" 90", true, 90);   // stoi skips leading whitespace
	CheckFov("abc", false, 0);
	CheckFov("", false, 0);
	CheckFov("  ", false, 0);

	// The item dispatch (1..6 select, 7 is exit-back, 9 is exit).
	CheckEq(static_cast<int>(vip_fov::ActionForItem(1)), static_cast<int>(vip_fov::SelectAction::SetFov),
			"item 1 selects");
	CheckEq(static_cast<int>(vip_fov::ActionForItem(6)), static_cast<int>(vip_fov::SelectAction::SetFov),
			"item 6 selects");
	CheckEq(static_cast<int>(vip_fov::ActionForItem(7)), static_cast<int>(vip_fov::SelectAction::OpenVipMenu),
			"item 7 opens the VIP menu");
	CheckEq(static_cast<int>(vip_fov::ActionForItem(9)), static_cast<int>(vip_fov::SelectAction::Ignore),
			"item 9 is ignored");
	CheckEq(static_cast<int>(vip_fov::ActionForItem(0)), static_cast<int>(vip_fov::SelectAction::SetFov),
			"item 0 selects (below 7, like the original)");

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
