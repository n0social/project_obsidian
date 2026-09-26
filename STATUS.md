# project_obsidian — have vs should-have

Living status as of 2026-08-11. Planning background lives in `OBSIDIAN_NOTES.md`.
Remaining work is tracked in `TODO.md`.

**v1 definition of done** (from the original plan): APK on arm64 tablet, user-supplied extracted 1.12.1 `Data/` loads, USB/BT K&M (relative look, WASD, hotkeys, text), connect to an external Vanilla 1.12 server, enter world and survive pause/resume, no Blizzard assets in the APK.

None of those playable criteria are met yet. The Android **shell** and the WoWee **port patch** exist.

## Blockers on this machine (2026-08-11)

| Blocker | Why it matters |
|---------|----------------|
| Device install + Data push | Debug APK exists; needs tablet/AVD then `install-and-debug.ps1` and `push-game-data.ps1 -Full` |
| NDK | Retargeted to **30.0.15729638** (installed) |

`retro_wow/RetroWoW 1.12.1/` **is** a real Vanilla 1.12.1 client (~5.3 GB of MPQs under `Data/`, plus `WoW.exe`). WoWee does **not** read MPQs at runtime. Extract with:

```powershell
.\scripts\extract-classic.ps1
```

That writes `WoWee/Data/expansions/classic/manifest.json`. Then `.\scripts\push-game-data.ps1 -Full` copies it onto the device.

## Have

| Piece | Where | Notes |
|-------|-------|--------|
| Android app shell | `android/` | `MainActivity` extends SDL2 `SDLActivity`, loads `c++_shared` + `wowee` |
| Vendored SDL Java | `org.libsdl.app` | SDL 2.32.10 glue for `SDLActivity` |
| Crash reporter | `CrashReporter.kt` | Java uncaught handler → `files/crash_reports/` |
| Data path env | `MainActivity` | `WOW_DATA_PATH` = `files/Data/`; writes `README_OBSIDIAN.txt` |
| CMake → WoWee | `android/app/src/main/cpp/CMakeLists.txt` | Expects `./WoWee` with the Android patch applied |
| Android port patch | `patches/wowee-android-port.patch` | Shared `libwowee.so`, Vulkan 1.1, no X11, SDL JNI keep-exports |
| Helper scripts | `scripts/` | setup / extract-classic / build / install / AVD / data-push |
| Vanilla 1.12.1 MPQs | `retro_wow/RetroWoW 1.12.1/Data/` | ~5.3 GB source |
| Classic extract | `WoWee/Data/expansions/classic/` | `manifest.json`, 79887 files, ~6.3 GB, 0 failed |
| Manifest policy | `AndroidManifest.xml` | Landscape, cleartext LAN, Vulkan optional, no touchscreen required |
| Branding | `drawable/obsidian_logo.png` | Launcher mark |
| Debug APK | `android/app/build/outputs/apk/debug/app-debug.apk` | 22.6 MB, arm64-v8a + x86_64, NDK 30 |

## Should have (v1) — not done

| Piece | Gap |
|-------|-----|
| `./WoWee` git clone | Done (imgui + vk-bootstrap). Android port rebased onto current CMake; classic extract restored. |
| vcpkg `arm64-android` (+ `x64-android`) | Done: glm, libiconv, openssl 3.6.3, sdl2 2.32.10, zlib 1.3.2 |
| NDK 30.0.15729638 | In use (retargeted from 27.2) |
| Debug APK that launches without native crash | APK built (`app-debug.apk`, 22.6 MB). Not yet installed/smoke-tested. |
| First-run “missing Data” UI | Today: empty SDL window / engine fail if manifest absent |
| Extracted classic `Data/` on device | `push-game-data.ps1 -Full` after PC extract |
| Relative mouse look verified | Patch/README claim it; never smoke-tested here |
| WASD / hotkeys / chat text | Same — SDL path exists, not proven |
| Auth → realm → char → world on LAN 1.12 | Needs desktop emulator + realmlist |
| Pause/resume Vulkan surface recreate | Patch does not fully solve lifecycle |
| Memory/streaming caps for tablet GPU | Not started |

## Doc / tree cleanup done this pass

- Removed unused GLES skeleton (`NativeBridge`, `NativeSurfaceView`, `obsidian_android.cpp`, `renderer_gles.*`, `activity_main.xml`)
- Added missing `@drawable/obsidian_logo` (manifest previously referenced a file that did not exist)
- Gradle/CMake now resolve `VCPKG_ROOT` from the environment instead of a dead hardcoded path
- `.gitignore` covers `retro_wow/` and `WoWee/`
- README / TODO brought in line with the code
