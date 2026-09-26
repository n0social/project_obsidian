# Project Obsydian `[Work in Progress]`

**Started:** July 30, 2026  
**Status:** Active development — not a finished product.

Android client for **Vanilla World of Warcraft 1.12**, built on the open-source [WoWee](https://github.com/Kelsidavis/WoWee) engine (World of Warcraft Engine Experiment).

You can boot, authenticate, and enter a 1.12 realm on a tablet in some cases, but session stability and character presentation still have known failures. Treat this repo as an experimental port, not a polished client.

Living detail: [STATUS.md](STATUS.md) · remaining work: [TODO.md](TODO.md)

## Based on WoWee

[WoWee](https://github.com/Kelsidavis/WoWee) is a clean-room C++ WoW client (Vulkan/SDL2) that talks to classic private-server backends. Obsydian does **not** replace WoWee; it ports and wraps that engine for Android.

| Item | Choice |
|------|--------|
| Engine | [Kelsidavis/WoWee](https://github.com/Kelsidavis/WoWee) |
| Expansion target | Vanilla **1.12.1** only |
| Input | Touch + USB / Bluetooth keyboard + mouse |
| Assets | BYO extracted `Data/` (no Blizzard assets shipped here) |
| Servers | External private 1.12 realms (client-only; tested against RetroWoW) |

## Repo layout

```
android/                         Obsydian Android app (SDLActivity + NDK → libwowee.so)
scripts/                         setup / build / install / emulator / data-push
patches/wowee-android-port.patch
STATUS.md                        have vs should-have (source of truth)
TODO.md                          remaining work
OBSIDIAN_NOTES.md                planning background
```

WoWee is **not vendored**. Clone it into this tree and apply the Android patch:

```powershell
.\scripts\setup-wowee.ps1
```

Equivalent manual steps:

```powershell
git clone --recurse-submodules https://github.com/Kelsidavis/WoWee.git WoWee
git -C WoWee apply ..\patches\wowee-android-port.patch
```

## Build

Prerequisites: Android SDK + NDK **30.0.15729638**, vcpkg with `arm64-android` (and `x64-android` for the Windows emulator) packages: openssl, sdl2, glm, zlib, libiconv. Set `VCPKG_ROOT`.

```powershell
.\scripts\setup-wowee.ps1
.\scripts\extract-classic.ps1          # RetroWoW 1.12.1 MPQs → WoWee\Data\expansions\classic
.\scripts\build-debug.ps1
.\scripts\install-and-debug.ps1
.\scripts\push-game-data.ps1 -SmokeTest
```

Latest debug APK (tracked in-repo for testers): [`obsidian-debug.apk`](obsidian-debug.apk)  
Build output path: `android\app\build\outputs\apk\debug\app-debug.apk`

ABIs: **arm64-v8a** (device) and **x86_64** (Windows emulator).

## Status

- [x] Project planning / roadmap
- [x] Android app shell + SDL2 `SDLActivity` host
- [x] CMake wiring to build WoWee as `libwowee.so`
- [x] Crash report hooks (Java)
- [x] Debug APK boots on device/emulator
- [x] Auth / realm / character select paths on RetroWoW (with VPN where required)
- [x] Enter world + basic terrain / entity load (unstable — see below)
- [ ] Stable world session (no Warden / peer kick)
- [ ] Correct in-world character appearance (hair and related geosets)
- [ ] Production-ready in-game UI and tablet QoL

## Known issues (actively working)

These are the current blockers. We are actively debugging and fixing them before treating the client as “playable”:

- **World disconnect shortly after Enter World** — server closes the socket (`peer_closed`), most often after Warden cheat-check / memory integrity replies. Maiev string-hash and HASH_REQUEST CR tables are improved; EndScene / MEM-check accuracy is still being hardened.
- **Character hair missing or wrong in-world** — char-select vs world geoset/texture paths differed; scalp overlays and hair connectors are being aligned with the classic CharSections / CharHairGeosets flow.
- **On-device client prepare** — Under Data, extract a 1.12.1 client folder or import a folder that already has manifest.json. Enter World saves the realm host typed under Sign in. Large extracts need free storage and time.
- **Occasional login / realm / create-flow glitches** — wrong screen skips, create preview quirks, or body proportion issues on some races/sexes.

Memory note: Graphics → **Texture resolution** downsamples at load time (default Medium/512 on tablet). That softens textures and cuts RAM — it does **not** tear meshes.

## What’s next (after the blockers above)

Once world stay-alive and character appearance are solid, the focus shifts to quality of life:

- **In-game UI** — usable tablet HUD, bag/action/target flows, clearer disconnect / reconnect messaging
- **Touch & camera** — look, move, and interact comfort on phones/tablets
- **Performance & streaming** — terrain load stalls, memory pressure, and VPN-sensitive sockets
- **Whatever else shows up in real play** — expect more classic-client edge cases (equipment display, spells, NPC interaction, audio, etc.) as sessions last longer

## Legal

This project ships **engine and app code only**. You must supply your own legally obtained 1.12 client data (extracted). No MPQs, no download links, no Blizzard assets.

`retro_wow/` (if present) is a local original-client tree for your own use. It is gitignored. WoWee still needs a **loose-file extract** (`Data/expansions/classic/manifest.json`), not raw MPQs.

## Specs

**Minimum Specs** (30–60 FPS)
Targeting 1280x800 or 1080p scaled rendering on medium graphics settings. Handles solo questing, world traversal, and 5-man dungeons without severe thermal throttling.

System on Chip (SoC): Qualcomm Snapdragon 870 or MediaTek Dimensity 8300 / 9000

CPU: Octa-Core (1x Prime Core @ 3.2 GHz Cortex-A77 + 3x Performance Cores @ 2.4 GHz + 4x Efficiency Cores)

GPU: Adreno 650 or Mali-G710 (Requires Vulkan 1.1+ support with stable extension handling)

System RAM: 8 GB LPDDR5

Internal Storage: 128 GB UFS 3.1 (Minimum read speeds of ~1,800 MB/s to prevent asset streaming hitches)

Thermal System: Passive Vapor Chamber / Large-surface area aluminum chassis

OS / Software: Android 11+

**Recommended Specs** (60–120 FPS)
Targeting Native 2K Resolution (2560x1600) on high settings. Handles dense capital cities, 20/40-man raid encounters, heavy add-on UIs, and continuous MPQ streaming with zero shader compilation micro-stutters.

System on Chip (SoC): Qualcomm Snapdragon 8 Gen 2 or Snapdragon 8+ Gen 1

CPU: Octa-Core (1x Cortex-X3 Prime Core @ 3.2 GHz + 4x Performance Cores @ 2.8 GHz + 3x Efficiency Cores)

GPU: Adreno 740 or Adreno 730 (Fully compliant Vulkan 1.3 support with low-latency pipeline caching)

System RAM: 12 GB – 16 GB LPDDR5X

Internal Storage: 256 GB UFS 4.0 (Ultra-fast read speeds of ~4,000 MB/s for instantaneous MPQ data archive inflation)

Thermal System: Dual-layer Vapor Chamber / Active Thermal Bypass Charging support

OS / Software: Android 13+

## License

Android shell and project docs: see repo files as they land.  
WoWee remains under its upstream license — see [WoWee](https://github.com/Kelsidavis/WoWee) (MIT).
