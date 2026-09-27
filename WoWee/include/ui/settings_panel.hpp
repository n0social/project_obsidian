#pragma once

#include "ui/ui_services.hpp"
#include <vulkan/vulkan.h>
#include <string>
#include <functional>
#include <cstdint>

namespace wowee {
namespace rendering { class Renderer; }
namespace audio { class AudioCoordinator; }
namespace ui {

class InventoryScreen;
class ChatPanel;

/**
 * Settings panel (extracted from GameScreen)
 *
 * Owns all settings UI rendering, settings state variables, and
 * graphics preset logic.  Save/load remains in GameScreen since
 * it serialises cross-cutting state (chat, quest tracker, etc.).
 */
class SettingsPanel {
public:
    // ---- Settings UI visibility flags (written by EscapeMenu / Escape key) ----
    bool showEscapeSettingsNotice = false;
    bool showSettingsWindow = false;
    bool settingsInit = false;

    // ---- Pending video / graphics settings ----
    bool pendingFullscreen = false;
    bool pendingVsync = true;
    int pendingResIndex = 0;
    int pendingResolutionWidth = 1920;
    int pendingResolutionHeight = 1080;
    bool displaySettingsLoaded_ = false;
    bool pendingShadows = true;
    float pendingShadowDistance = 300.0f;
    float pendingViewDistance = 1200.0f;
    bool pendingWaterRefraction = true;
    int pendingBrightness = 50; // 0-100, maps to 0.0-2.0 (50 = 1.0 default)

    // ---- Pending audio settings ----
    int pendingMasterVolume = 100;
    int pendingMusicVolume = 30;
    int pendingAmbientVolume = 100;
    int pendingBellVolume = 50;
    int pendingUiVolume = 100;
    int pendingCombatVolume = 100;
    int pendingSpellVolume = 100;
    int pendingMovementVolume = 100;
    int pendingFootstepVolume = 100;
    int pendingNpcVoiceVolume = 100;
    int pendingMountVolume = 70;
    int pendingActivityVolume = 100;
    bool pendingCharacterSpeech = true;

    // ---- Pending camera / controls ----
    float pendingMouseSensitivity = 0.2f;
    bool pendingInvertMouse = false;
    bool pendingRelativeMouse = true; // WoW-style capture look on LMB/RMB hold
    bool pendingExtendedZoom = false;
    float pendingCameraStiffness = 30.0f;  // Camera smooth speed (higher = tighter, less sway)
    float pendingPivotHeight = 1.6f;       // Camera pivot height above feet (lower = less detached feel)
    bool pendingSmoothCameraFollow = false; // Keep lerping while turning (floaty, detached follow)
    float pendingFov = 70.0f;  // degrees, default matches WoW's ~70° horizontal FOV

    // ---- Pending UI / interface ----
    int pendingUiOpacity = 65;
    float pendingWindowUiScale = 1.0f;
    bool pendingMinimapRotate = false;
    bool pendingMinimapSquare = false;
    bool pendingMinimapNpcDots = false;
    bool pendingShowMinimapClock = false;
    bool pendingShowMinimapCoordinates = false;
    bool pendingShowLatencyMeter = true;
    bool pendingSeparateBags = true;
    bool pendingShowKeyring = true;
    float pendingBagScale = 1.0f;
    bool pendingShowMicroMenu = true;

    // ---- Pending gameplay ----
    bool pendingAutoLoot = false;
    bool pendingAutoSellGrey = false;
    bool pendingAutoRepair = false;
    bool pendingIdleCameraOrbit = true;

    // ---- Pending soundtrack ----
    bool pendingUseOriginalSoundtrack = true;

    // ---- Pending buff bar layout ----
    // Multiplier for the buff/debuff bar icons, on top of the automatic
    // resolution scaling (0.75–1.5).
    float pendingBuffBarScale = 1.0f;

    // ---- Pending action bar layout ----
    bool pendingShowActionBar2 = false;  // Show bottom-left extra action bar above main bar
    float pendingActionBarScale = 1.0f;  // Multiplier for action bar slot size (0.5–1.5)
    float pendingActionBar2OffsetX = 0.0f;  // Horizontal offset from default center position
    float pendingActionBar2OffsetY = 0.0f;  // Vertical offset from default (above bar 1)
    bool pendingShowRightBar = false;   // Right-edge vertical action bar (FrameXML page 3)
    bool pendingShowLeftBar  = false;   // Left-edge vertical action bar (FrameXML page 4)
    float pendingRightBarOffsetY = 0.0f;  // Vertical offset from screen center
    float pendingLeftBarOffsetY  = 0.0f;  // Vertical offset from screen center

    // ---- Pending graphics quality ----
    int pendingGroundClutterDensity = 100;
    int pendingAntiAliasing = 0;  // 0=Off, 1=2x, 2=4x, 3=8x
    bool pendingFXAA = false;     // FXAA post-process (combinable with MSAA)
    bool pendingNormalMapping = true;   // on by default
    float pendingNormalMapStrength = 0.8f;  // 0.0-2.0
    bool pendingPOM = true;             // on by default
    int pendingPOMQuality = 1;          // 0=Low(16), 1=Medium(32), 2=High(64)
    bool pendingFSR = false;
    int pendingUpscalingMode = 0;       // 0=Off, 1=FSR1, 2=FSR3
    int pendingFSRQuality = 3;          // 0=UltraQuality, 1=Quality, 2=Balanced, 3=Native(100%)
    float pendingFSRSharpness = 1.6f;
    float pendingFSR2JitterSign = 0.38f;
    float pendingFSR2MotionVecScaleX = 1.0f;
    float pendingFSR2MotionVecScaleY = 1.0f;
    bool pendingAMDFramegen = false;

    // ---- Graphics quality presets ----
    enum class GraphicsPreset : int {
        CUSTOM = 0,
        LOW = 1,
        MEDIUM = 2,
        HIGH = 3,
        ULTRA = 4
    };
    GraphicsPreset currentGraphicsPreset = GraphicsPreset::CUSTOM;
    GraphicsPreset pendingGraphicsPreset = GraphicsPreset::CUSTOM;

    // ---- Applied-once flags (used by GameScreen::render() one-time-apply blocks) ----
    bool fsrSettingsApplied_ = false;
    float uiOpacity_ = 0.65f;  // UI element transparency (0.0 = fully transparent, 1.0 = fully opaque)
    bool minimapRotate_ = false;
    bool minimapSquare_ = false;
    bool minimapNpcDots_ = false;
    bool showMinimapClock_ = false;
    bool showMinimapCoordinates_ = false;
    bool showLatencyMeter_ = true;           // Show server latency indicator
    bool minimapSettingsApplied_ = false;
    bool volumeSettingsApplied_ = false;  // True once saved volume settings applied to audio managers
    bool msaaSettingsApplied_ = false;   // True once saved MSAA setting applied to renderer
    bool fxaaSettingsApplied_ = false;   // True once saved FXAA setting applied to renderer
    bool lightingSettingsApplied_ = false; // True once saved shadows/brightness are applied
    bool waterRefractionApplied_ = false;
    bool normalMapSettingsApplied_ = false;  // True once saved normal map/POM settings applied

    // ---- Mute state: mute bypasses master volume without touching slider values ----
    bool soundMuted_ = false;
    float preMuteVolume_ = 1.0f;  // AudioEngine master volume before muting

    // ---- Config toggles (read by GameScreen rendering, edited by Interface tab) ----
    float nameplateScale_ = 1.0f; // Scale multiplier for nameplate bar dimensions
    bool showFriendlyNameplates_ = true;  // Shift+V toggles friendly player nameplates
    bool showDPSMeter_ = false;
    bool showCooldownTracker_ = false;
    bool showRareTracker_ = false;  // Mark nearby spawned rares/rare-elites on both maps
    bool showChestTracker_ = false; // Mark nearby spawned non-gather chests on both maps
    bool damageFlashEnabled_ = true;
    bool lowHealthVignetteEnabled_ = true; // Persistent pulsing red vignette below 20% HP

    // ---- Public methods ----

    /// Render the settings window (call from GameScreen::render)
    void renderSettingsWindow(InventoryScreen& inventoryScreen, ChatPanel& chatPanel,
                              std::function<void()> saveCallback);

    /// Apply audio volume levels to all audio coordinator sound managers
    void applyAudioVolumes(audio::AudioCoordinator* ac);

    /// Apply the persisted global ImGui window scale without compounding ratios.
    void applyWindowUiScale();

    /// Return the platform-specific settings file path
    static std::string getSettingsPath();

    /// Set services (dependency injection)
    void setServices(const UIServices& services) { services_ = services; }

private:
    UIServices services_;  // Injected service references
    float appliedWindowUiScale_ = 1.0f;
    bool windowUiScaleEditing_ = false;

    // Keybinding customization (private — only used in Controls tab)
    int pendingRebindAction_ = -1;  // -1 = not rebinding, otherwise action index
    bool awaitingKeyPress_ = false;

    // Settings tab rendering
    void renderSettingsInterfaceTab(std::function<void()> saveCallback);
    void renderSettingsGameplayTab(InventoryScreen& inventoryScreen,
                                   std::function<void()> saveCallback);
    void renderSettingsControlsTab(std::function<void()> saveCallback);
    void renderSettingsAudioTab(std::function<void()> saveCallback);
    void renderSettingsAboutTab();
    void applyGraphicsPreset(GraphicsPreset preset);
    void updateGraphicsPresetFromCurrentSettings();
};

/// The 1.12 Video and Sound buttons ask for this. The vanilla interface
/// options stay their own frame; this opens the client's settings window.
void requestClientSettings();
bool consumeClientSettingsRequest();

} // namespace ui
} // namespace wowee
