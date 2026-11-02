// vip_fov — implementation of the plugin's own s2sdk guards (see guards.hpp).

#include "guards.hpp"

#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <cstdint>
#include <exception>
#include <string>

#include "common.hpp"

namespace vip_fov {
namespace {

// The same three flags (and the same state machine) the sibling pure-s2sdk plugins keep:
//   g_mapReady        a map is live - the only state in which a client/entity/schema call is legal
//   g_mapStartSeen    an OnMapStart arrived since OnPluginStart / the last OnMapEnd
//   g_mapEndSeen      an OnMapEnd arrived since OnPluginStart
bool g_mapReady = false;
bool g_mapStartSeen = false;
bool g_mapEndSeen = false;
bool g_listenersRegistered = false;

MapLifecycleCallback g_onMapStart = nullptr;
MapLifecycleCallback g_onMapEnd = nullptr;

// s2sdk listeners: plain function pointers called on the game thread. Nothing here may throw and
// no client or entity call is made - only state changes.
void OnMapStartListener() {
	// The engine calls this once the new map and its entity system exist: readiness is granted here
	// for everything that follows (PITFALLS B1). A plugin loaded into a running map never sees this
	// callback - PollMapGate() covers it.
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down and every
	// client query crashes - the gate is dropped first and the hook only clears state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// class + '\0' + field, so one map holds every field without an allocation per lookup key.
std::string OffsetKey(const char* className, const char* fieldName) {
	std::string key(className);
	key.push_back('\0');
	key.append(fieldName);
	return key;
}

// Only successful offsets are remembered; a -1 (unknown field, or a lookup that raced the map
// window) stays retryable instead of poisoning the field for the session (PITFALLS C8).
struct SchemaOffsetCache {
	std::string key;
	int32_t offset;
};

SchemaOffsetCache g_schemaOffsets[8]{};
int g_schemaOffsetCount = 0;
constexpr int kMaxCachedOffsets = static_cast<int>(sizeof(g_schemaOffsets) / sizeof(g_schemaOffsets[0]));

void ResetSchemaCache() {
	for (SchemaOffsetCache& entry : g_schemaOffsets) {
		entry = SchemaOffsetCache{};
	}
	g_schemaOffsetCount = 0;
}

int32_t LookupSchemaOffset(const char* className, const char* fieldName) {
	if (__s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		return s2sdk::GetSchemaOffset(plg::string(className), plg::string(fieldName)); // -1: not found
	} catch (...) {
		return -1;
	}
}

} // namespace

// --- map gate ---------------------------------------------------------------------------------

void SetMapLifecycleCallbacks(MapLifecycleCallback onMapStart, MapLifecycleCallback onMapEnd) {
	g_onMapStart = onMapStart;
	g_onMapEnd = onMapEnd;
}

void InitMapGate() {
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	if (g_listenersRegistered) {
		return;
	}
	// A registration without its matching Unregister would leave a callback in the unloaded .so
	// after `plg reload` (PITFALLS A7), so all four slots are required before ANY registration -
	// the same pair rule the vip callbacks follow. A partially present API leaves the gate shut
	// for the whole instance (fail safe, PITFALLS A1: log and stay inactive, never return Failed).
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapStart_Unregister == nullptr ||
		__s2sdk_OnMapEnd_Register == nullptr || __s2sdk_OnMapEnd_Unregister == nullptr) {
		LogTaggedError("the s2sdk map listeners are unavailable (Register/Unregister pair missing) - "
					   "client calls stay disabled");
		return;
	}
	s2sdk::OnMapStart_Register(&OnMapStartListener);
	s2sdk::OnMapEnd_Register(&OnMapEndListener);
	g_listenersRegistered = true;
}

void PollMapGate() {
	if (!g_listenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first OnPluginUpdate after
	// OnPluginStart proves a live map (the host pumps updates from the server frame). After an
	// OnMapEnd only a new OnMapStart may re-open the gate - the host keeps pumping updates through
	// a changelevel, so "no OnMapEnd seen" must not be enough once one was seen.
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

void ShutdownMapGate() {
	if (g_listenersRegistered) {
		g_listenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister) {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister) {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		}
	}
	// Drop readiness last: nothing can reach a client through this plugin after OnPluginEnd.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
}

bool IsMapReady() {
	return g_mapReady;
}

// --- entity handles (PITFALLS C1/C2) ----------------------------------------------------------

bool IsUsableEntityHandle(int32_t handle) {
	if (!g_mapReady || handle == 0 || handle == -1 || __s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsValidEntHandle(handle);
	} catch (...) {
		return false;
	}
}

// --- client checks (PITFALLS C2) --------------------------------------------------------------

bool HasController(int32_t iSlot) {
	if (!IsSlotInRange(iSlot) || !g_mapReady) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_PlayerSlotToEntHandle == nullptr || __s2sdk_IsValidEntHandle == nullptr ||
		__s2sdk_IsValidEntPointer == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientConnected(iSlot) || !s2sdk::IsClientInGame(iSlot) ||
			s2sdk::IsClientSourceTV(iSlot)) {
			return false;
		}
		// The controller entity: the pointer must exist, its handle must be usable and the engine
		// must still recognise both (C2: never pass a stale handle to a schema write).
		void* controller = s2sdk::PlayerSlotToEntPointer(iSlot);
		if (controller == nullptr) {
			return false;
		}
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(iSlot);
		return IsUsableEntityHandle(handle) && s2sdk::IsValidEntPointer(controller);
	} catch (...) {
		return false;
	}
}

// --- owner identity (stale menu request guard) ------------------------------------------------

bool CaptureOwner(int32_t iSlot, OwnerIdentity& out) {
	out = OwnerIdentity{};
	if (!IsSlotInRange(iSlot) || !HasController(iSlot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return false;
	}
	try {
		const uint64_t steamId = s2sdk::GetClientSteamID64(iSlot);
		if (steamId == 0) {
			return false; // s2sdk's "no player" answer: never apply a request to it
		}
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(iSlot);
		if (!IsUsableEntityHandle(handle)) {
			return false;
		}
		// SteamID64 pins the account, the controller handle carries the entity serial - the two
		// identifiers together survive a slot being re-used by another player between frames.
		out = OwnerIdentity{iSlot, steamId, handle};
		return true;
	} catch (...) {
		return false;
	}
}

bool OwnerMatches(const OwnerIdentity& owner) {
	if (!owner.IsSet()) {
		return false;
	}
	OwnerIdentity current;
	if (!CaptureOwner(owner.slot, current)) {
		return false;
	}
	return current.steamId == owner.steamId && current.controllerHandle == owner.controllerHandle;
}

// --- schema offsets (PITFALLS C8) -------------------------------------------------------------

int32_t SchemaOffset(const char* className, const char* fieldName) {
	if (!g_mapReady || className == nullptr || fieldName == nullptr || *className == '\0' ||
		*fieldName == '\0' || __s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	try {
		const std::string key = OffsetKey(className, fieldName);
		for (int i = 0; i < g_schemaOffsetCount; ++i) {
			if (g_schemaOffsets[i].key == key) {
				return g_schemaOffsets[i].offset;
			}
		}
		const int32_t offset = LookupSchemaOffset(className, fieldName);
		if (offset >= 0 && g_schemaOffsetCount < kMaxCachedOffsets) {
			g_schemaOffsets[g_schemaOffsetCount++] = SchemaOffsetCache{key, offset};
		}
		return offset;
	} catch (...) {
		return -1; // the cache allocates; a spawn/menu handler may not unwind into the engine
	}
}

void ClearSchemaOffsets() {
	ResetSchemaCache();
}

// --- the desired-FOV write --------------------------------------------------------------------

bool SetDesiredFov(int32_t iSlot, int iFov) {
	// The map gate, the slot and the whole controller identity chain first: s2sdk's SetEntData
	// does not validate its handle and a disconnected slot must never reach it (PITFALLS B1/C2).
	if (!HasController(iSlot)) {
		return false;
	}
	const int32_t offset = SchemaOffset(kFovClass, kFovField);
	if (offset < 0 || __s2sdk_SetEntData == nullptr) {
		return false;
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(iSlot);
		if (!IsUsableEntityHandle(handle)) {
			return false;
		}
		// Raw handle write: 4 bytes (SCHEMA_FIELD(uint)), changeState = true (the original's
		// SetStateChanged) and chainOffset -1, "the entity itself" (PITFALLS C5/C6).
		s2sdk::SetEntData(handle, offset, iFov, kFovFieldSize, /*changeState=*/true, /*chainOffset=*/-1);
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace vip_fov
