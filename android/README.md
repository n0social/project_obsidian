# project_obsidian Android

Vanilla 1.12 client shell on [WoWee](https://github.com/Kelsidavis/WoWee), hosted by SDL2 `SDLActivity`.

Entry: `MainActivity` loads `c++_shared` + `wowee`. Native build is CMake `add_subdirectory(../../WoWee)` after `patches/wowee-android-port.patch`.

## Build / install / data

From repo root:

```powershell
.\scripts\setup-wowee.ps1
.\scripts\build-debug.ps1
.\scripts\install-and-debug.ps1
.\scripts\push-game-data.ps1 -SmokeTest   # path check only
.\scripts\push-game-data.ps1 -Full        # real ~6GB classic extract
```

APK: `android\app\build\outputs\apk\debug\app-debug.apk`

ABIs: `arm64-v8a` (device), `x86_64` (Windows emulator).

## Runtime paths (device)

| Path | Purpose |
|------|---------|
| `/data/data/com.obsidian.client/files/Data/` | Extracted game data (`WOW_DATA_PATH`) |
| `/data/data/com.obsidian.client/files/crash_reports/` | Java crash reports |

Pull crashes:

```powershell
adb exec-out run-as com.obsidian.client sh -c "cd files/crash_reports && tar cf - ." > crashes.tar
```

## Controls

- USB/BT keyboard + mouse preferred
- Relative look is intended via SDL (`SDL_SetRelativeMouseMode`) — not yet smoke-tested
- Soft keyboard stays hidden

## Debug logcat

```powershell
adb logcat -v threadtime Obsidian:V ObsidianCrash:V SDL:V Wowee:V *:S
```
