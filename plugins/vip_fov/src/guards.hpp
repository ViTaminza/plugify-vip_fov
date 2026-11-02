// vip_fov — the plugin's own s2sdk safety guards (PITFALLS B1/B2, C1/C2, C8).
//
// The plugin calls s2sdk directly (docs/PLUGIN_STANDARD.md §7: no pisexcompat), so every rule the
// old wrappers enforced lives here:
//
//   * Map gate (B1/B2). No client, entity or schema call is made before the first map and none in
//     the window from OnMapEnd until the next OnMapStart: the client list is torn down there and
//     s2sdk's client lookups read an emptied list, while the serializer map GetSchemaOffset walks
//     does not exist before the first map. InitMapGate() registers the s2sdk OnMapStart/OnMapEnd
//     listeners (OnPluginStart), PollMapGate() latches readiness on OnMapStart *or* on the first
//     OnPluginUpdate of a plugin loaded into a running map (when no OnMapEnd was seen), and
//     ShutdownMapGate() removes the listeners again (OnPluginEnd, PITFALLS A7).
//     SetMapLifecycleCallbacks() lets the menu controller drop its per-map state from the very
//     same two events, without the gate having to know about the menu layer.
//   * Client checks (C2). s2sdk exports do not validate their arguments, so HasController()
//     layers the map gate, the slot range, IsClientConnected, IsClientInGame, !IsClientSourceTV,
//     a controller entity pointer *and* a usable, valid handle. The original wrote
//     m_iDesiredFOV on the controller only (CCSPlayerController::FromSlot plus a null check), so a
//     dead player's menu selection stays legal: no pawn/alive layer is required for this write.
//   * Owner identity (the menu layer's stale-request guard). A menu action or a queued display is
//     not allowed to apply to whoever happens to occupy the slot when it runs: CaptureOwner()
//     records the SteamID64 and the controller handle (index | serial << 15, so it includes the
//     generation) while the map is up, and OwnerMatches() re-captures and compares both. No raw
//     entity pointer is kept across frames (PITFALLS C3). GetClientSteamID64() answers 0 for a
//     player that is gone, so 0 is "not verified" and the request is dropped.
//   * Entity handles (C1). An entity int is a CEntityHandle (index | serial << 15): 0 is the world,
//     -1 is INVALID_EHANDLE_INDEX, and a *valid* handle may be negative once the serial counter
//     passes 65535 - the numeric filter is not "handle > 0".
//   * Schema offsets (C8). GetSchemaOffset is only called while the gate is open; only a
//     successful field offset (>= 0) is cached, and ClearSchemaOffsets() (OnPluginEnd) makes a
//     reload resolve the field again instead of reusing an offset of the previous instance.
//   * Field write. SetDesiredFov() re-validates slot/controller in the same tick and then writes
//     m_iDesiredFOV with the raw, handle-based SetEntData: 4 bytes, changeState = true (the
//     original's explicit SetStateChanged) and chainOffset -1 ("the entity itself"). The raw write
//     avoids the typed-schema poison of PITFALLS C6 and the silent CS2BadList no-ops of C7.

#pragma once

#include <cstdint>

namespace vip_fov {

// The verified owner of a queued menu action/display: the slot plus the two identifiers that prove
// the same player still occupies it. The controller *handle* carries the entity serial
// (index | serial << 15), so it is a generation check; the SteamID64 pins the account. No raw
// entity pointer is ever stored across frames (PITFALLS C3).
struct OwnerIdentity {
	int32_t slot = -1;
	uint64_t steamId = 0;
	int32_t controllerHandle = 0;

	constexpr bool IsSet() const {
		return slot >= 0 && steamId != 0 && controllerHandle != 0 && controllerHandle != -1;
	}
};

// A plugin map-lifecycle hook, called by the OnMapStart/OnMapEnd listeners after the gate state has
// been updated. Plain function pointer (s2sdk listeners are C function pointers anyway) and
// noexcept in practice: the map-end hook runs while the client list is being torn down and must
// only drop state.
using MapLifecycleCallback = void (*)();

// Installs (or clears) the plugin's per-map state hooks; pass nullptr to drop them on unload.
void SetMapLifecycleCallbacks(MapLifecycleCallback onMapStart, MapLifecycleCallback onMapEnd);

// Registers the OnMapStart/OnMapEnd listeners. Both Register+Unregister pairs must be present
// before anything is registered (a dangling callback would survive the unload otherwise); with a
// partial API the gate simply stays shut. Touches no client; safe in OnPluginStart.
void InitMapGate();

// Per-frame pump (OnPluginUpdate): opens the gate for a plugin that loaded into a running map.
void PollMapGate();

// Removes the map listeners and closes the gate; called from OnPluginEnd so `plg reload` leaves
// nothing pointing into the unloaded library (PITFALLS A7).
void ShutdownMapGate();

// True while a map is up and client/entity/schema calls are safe.
bool IsMapReady();

// A usable (non-world, non-invalid, live) entity handle, only while a map is up: IsValidEntHandle
// walks the entity system, which is null before the first map (PITFALLS B1). The numeric filter
// drops 0 and -1 only: a valid handle can be negative once the serial counter passes 65535.
bool IsUsableEntityHandle(int32_t handle);

// The original's "CCSPlayerController::FromSlot(iSlot) != nullptr" plus every s2sdk guard the old
// pisex::IsClientReady()/SetEntSchema pair needed: a live map, a slot in range, a connected
// in-game client that is not the SourceTV proxy, and a controller entity with both a pointer and
// a valid handle. Nothing below a failed layer reaches s2sdk.
bool HasController(int32_t iSlot);

// Records the slot's verified owner. false (and out is cleared) when there is no live map, no
// controller, the s2sdk SteamID binding is missing or the engine answers 0 - the caller must drop
// the request rather than apply it to an unverified player.
bool CaptureOwner(int32_t iSlot, OwnerIdentity& out);

// Re-captures the slot and compares SteamID64 *and* controller handle/generation with `owner`.
// false for an unset owner, a departed player, a re-used slot or a different controller instance.
bool OwnerMatches(const OwnerIdentity& owner);

// The desired-FOV write of the original (m_iDesiredFOV = iFov followed by SetStateChanged), on
// the validated controller handle. false when there is no map, no controller, no schema offset or
// when the s2sdk bindings are missing; the caller has no state to roll back.
bool SetDesiredFov(int32_t iSlot, int iFov);

// The map-gated schema lookup: -1 while the gate is closed, only offsets >= 0 cached.
int32_t SchemaOffset(const char* className, const char* fieldName);

// Called from OnPluginEnd so a reload resolves the field against the live map again (C8).
void ClearSchemaOffsets();

} // namespace vip_fov
