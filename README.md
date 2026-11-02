# vip_fov (Plugify)

[VIP] FOV: the selectable 'FOV' feature opens a menu of the group's comma-separated FOV values; the choice is stored in the 'FOV_Value' cookie and applied to m_iDesiredFOV on spawn. Ported from the Pisex VIP core module VIP_FOV (vip_fov) to pure s2sdk (no pisexcompat); the menu type follows the player's own '!menu' choice through the optional hudmenu plugin. Requires the vip core.

Plugify port (fork) of **VIP_FOV ([VIP] FOV)** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip-modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), vip (>=1.0.0), hudmenu (>=1.0.0, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `vip_fov-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_fov`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

