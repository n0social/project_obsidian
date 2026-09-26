// ============================================================
// SettingsPanel — extracted from GameScreen
// Owns all settings UI rendering, settings state, and
// graphics preset logic.
// ============================================================
#include "ui/settings_panel.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/chat_panel.hpp"
#include "ui/keybinding_manager.hpp"
#include "core/application.hpp"
#include "core/config_paths.hpp"
#include "core/logger.hpp"
#include "core/version.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/minimap.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "game/zone_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace wowee { namespace ui {

void SettingsPanel::renderSettingsInterfaceTab(std::function<void()> saveCallback) {
ImGui::Spacing();
ImGui::BeginChild("InterfaceSettings", ImVec2(0, -1), true);

ImGui::SeparatorText("Window UI");
ImGui::Spacing();
ImGui::SetNextItemWidth(200.0f);
ImGui::SliderFloat("Window UI Scale", &pendingWindowUiScale, 0.75f, 1.5f, "%.2fx");
if (ImGui::IsItemActive()) {
    // Keep the window and slider stationary while the pointer is dragging.
    windowUiScaleEditing_ = true;
}
if (ImGui::IsItemDeactivatedAfterEdit()) {
    windowUiScaleEditing_ = false;
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(fonts, controls, and spacing)");
ImGui::Spacing();

ImGui::SeparatorText("Buff Bar");
ImGui::Spacing();
ImGui::SetNextItemWidth(200.0f);
if (ImGui::SliderFloat("Buff Bar Scale", &pendingBuffBarScale, 0.75f, 1.5f, "%.2fx")) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(on top of automatic resolution scaling)");
ImGui::Spacing();

ImGui::SeparatorText("Action Bars");
ImGui::Spacing();
ImGui::SetNextItemWidth(200.0f);
if (ImGui::SliderFloat("Action Bar Scale", &pendingActionBarScale, 0.5f, 1.5f, "%.2fx")) {
    saveCallback();
}
ImGui::Spacing();

if (ImGui::Checkbox("Show Bottom Left Bar", &pendingShowActionBar2)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(client action page 6)");

if (pendingShowActionBar2) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Bottom Left Bar Position Offset");
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Horizontal##bar2x", &pendingActionBar2OffsetX, -600.0f, 600.0f, "%.0f px")) {
        saveCallback();
    }
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Vertical##bar2y", &pendingActionBar2OffsetY, -400.0f, 400.0f, "%.0f px")) {
        saveCallback();
    }
    if (ImGui::Button("Reset Position##bar2")) {
        pendingActionBar2OffsetX = 0.0f;
        pendingActionBar2OffsetY = 0.0f;
        saveCallback();
    }
}

ImGui::Spacing();
if (ImGui::Checkbox("Show Right Side Bar", &pendingShowRightBar)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(client action page 3)");
if (pendingShowRightBar) {
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Vertical Offset##rbar", &pendingRightBarOffsetY, -400.0f, 400.0f, "%.0f px")) {
        saveCallback();
    }
}

ImGui::Spacing();
if (ImGui::Checkbox("Show Left Side Bar", &pendingShowLeftBar)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(client action page 4)");
if (pendingShowLeftBar) {
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Vertical Offset##lbar", &pendingLeftBarOffsetY, -400.0f, 400.0f, "%.0f px")) {
        saveCallback();
    }
}

ImGui::Spacing();
if (ImGui::Checkbox("Show Micro Menu Buttons", &pendingShowMicroMenu)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(bottom-left window shortcuts)");

ImGui::Spacing();
ImGui::SeparatorText("Nameplates");
ImGui::Spacing();
ImGui::SetNextItemWidth(200.0f);
if (ImGui::SliderFloat("Nameplate Scale", &nameplateScale_, 0.5f, 2.0f, "%.2fx")) {
    saveCallback();
}

ImGui::Spacing();
ImGui::SeparatorText("Network");
ImGui::Spacing();
if (ImGui::Checkbox("Show Latency Meter", &pendingShowLatencyMeter)) {
    showLatencyMeter_ = pendingShowLatencyMeter;
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(ms indicator near minimap)");

if (ImGui::Checkbox("Show DPS/HPS Meter", &showDPSMeter_)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(damage/healing per second above action bar)");

if (ImGui::Checkbox("Show Cooldown Tracker", &showCooldownTracker_)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(active spell cooldowns near action bar)");

if (ImGui::Checkbox("Show Rare Tracker", &showRareTracker_)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(mark nearby spawned rares on the world map and minimap)");

if (ImGui::Checkbox("Show Chest Tracker", &showChestTracker_)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(mark nearby spawned loot chests on the world map and minimap)");

ImGui::Spacing();
ImGui::SeparatorText("Screen Effects");
ImGui::Spacing();
if (ImGui::Checkbox("Damage Flash", &damageFlashEnabled_)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(red vignette on taking damage)");

if (ImGui::Checkbox("Low Health Vignette", &lowHealthVignetteEnabled_)) {
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(pulsing red edges below 20%% HP)");

ImGui::EndChild();
}

void SettingsPanel::renderSettingsGameplayTab(InventoryScreen& inventoryScreen,
                                                  std::function<void()> saveCallback) {
    auto* renderer = services_.renderer;
ImGui::Spacing();
ImGui::BeginChild("GameplaySettings", ImVec2(0, -1), true);

ImGui::Text("Controls");
ImGui::Separator();
if (ImGui::SliderFloat("Mouse Sensitivity", &pendingMouseSensitivity, 0.05f, 1.0f, "%.2f")) {
    if (renderer) {
        if (auto* cameraController = renderer->getCameraController()) {
            cameraController->setMouseSensitivity(pendingMouseSensitivity);
        }
    }
    saveCallback();
}
if (ImGui::Checkbox("Invert Mouse", &pendingInvertMouse)) {
    if (renderer) {
        if (auto* cameraController = renderer->getCameraController()) {
            cameraController->setInvertMouse(pendingInvertMouse);
        }
    }
    saveCallback();
}
if (ImGui::Checkbox("Extended Camera Zoom", &pendingExtendedZoom)) {
    if (renderer) {
        if (auto* cameraController = renderer->getCameraController()) {
            cameraController->setExtendedZoom(pendingExtendedZoom);
        }
    }
    saveCallback();
}
if (ImGui::SliderFloat("Camera Stiffness", &pendingCameraStiffness, 5.0f, 100.0f, "%.0f")) {
    if (renderer) {
        if (auto* cameraController = renderer->getCameraController()) {
            cameraController->setCameraSmoothSpeed(pendingCameraStiffness);
        }
    }
    saveCallback();
}
ImGui::SetItemTooltip("Higher = tighter camera with less sway. Default: 30");
if (ImGui::Checkbox("Smooth Camera Follow", &pendingSmoothCameraFollow)) {
    if (renderer) {
        if (auto* cameraController = renderer->getCameraController()) {
            cameraController->setSmoothCameraFollow(pendingSmoothCameraFollow);
        }
    }
    saveCallback();
}
ImGui::SetItemTooltip("Camera keeps drifting toward its position even while turning,\n"
                      "for a floaty, slightly detached follow. Off = turning moves the\n"
                      "camera 1:1 with your input.");
if (ImGui::SliderFloat("Camera Pivot Height", &pendingPivotHeight, 0.0f, 3.0f, "%.1f")) {
    if (renderer) {
        if (auto* cameraController = renderer->getCameraController()) {
            cameraController->setPivotHeight(pendingPivotHeight);
        }
    }
    saveCallback();
}
ImGui::SetItemTooltip("Height of camera orbit point above feet. Lower = less detached feel. Default: 1.8");
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Allow the camera to zoom out further than normal");

if (ImGui::Checkbox("Idle Camera Orbit", &pendingIdleCameraOrbit)) {
    if (renderer) {
        if (auto* cameraController = renderer->getCameraController()) {
            cameraController->setIdleOrbitEnabled(pendingIdleCameraOrbit);
        }
    }
    saveCallback();
}

if (ImGui::SliderFloat("Field of View", &pendingFov, 45.0f, 110.0f, "%.0f°")) {
    if (renderer) {
        if (auto* camera = renderer->getCamera()) {
            camera->setFov(pendingFov);
        }
    }
    saveCallback();
}
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Camera field of view in degrees (default: 70)");

ImGui::Spacing();
ImGui::Spacing();

ImGui::Text("Interface");
ImGui::Separator();
if (ImGui::SliderInt("UI Opacity", &pendingUiOpacity, 20, 100, "%d%%")) {
    uiOpacity_ = static_cast<float>(pendingUiOpacity) / 100.0f;
    saveCallback();
}
if (ImGui::Checkbox("Rotate Minimap", &pendingMinimapRotate)) {
    // Force north-up minimap.
    minimapRotate_ = false;
    pendingMinimapRotate = false;
    if (renderer) {
        if (auto* minimap = renderer->getMinimap()) {
            minimap->setRotateWithCamera(false);
        }
    }
    saveCallback();
}
if (ImGui::Checkbox("Square Minimap", &pendingMinimapSquare)) {
    minimapSquare_ = pendingMinimapSquare;
    if (renderer) {
        if (auto* minimap = renderer->getMinimap()) {
            minimap->setSquareShape(minimapSquare_);
        }
    }
    saveCallback();
}
if (ImGui::Checkbox("Show Nearby NPC Dots", &pendingMinimapNpcDots)) {
    minimapNpcDots_ = pendingMinimapNpcDots;
    saveCallback();
}
if (ImGui::Checkbox("Show Minimap Clock", &pendingShowMinimapClock)) {
    showMinimapClock_ = pendingShowMinimapClock;
    saveCallback();
}
if (ImGui::Checkbox("Show Minimap Coordinates", &pendingShowMinimapCoordinates)) {
    showMinimapCoordinates_ = pendingShowMinimapCoordinates;
    saveCallback();
}
// Zoom controls
ImGui::Text("Minimap Zoom:");
ImGui::SameLine();
if (ImGui::Button("  -  ")) {
    if (renderer) {
        if (auto* minimap = renderer->getMinimap()) {
            minimap->zoomOut();
            saveCallback();
        }
    }
}
ImGui::SameLine();
if (ImGui::Button("  +  ")) {
    if (renderer) {
        if (auto* minimap = renderer->getMinimap()) {
            minimap->zoomIn();
            saveCallback();
        }
    }
}

ImGui::Spacing();
ImGui::Text("Loot");
ImGui::Separator();
if (ImGui::Checkbox("Auto Loot", &pendingAutoLoot)) {
    saveCallback();  // per-frame sync applies pendingAutoLoot to gameHandler
}
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Automatically pick up all items when looting");
if (ImGui::Checkbox("Auto Sell Greys", &pendingAutoSellGrey)) {
    saveCallback();
}
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Automatically sell all grey (poor quality) items when opening a vendor");
if (ImGui::Checkbox("Auto Repair", &pendingAutoRepair)) {
    saveCallback();
}
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Automatically repair all damaged equipment when opening an armorer vendor");

ImGui::Spacing();
ImGui::Text("Bags");
ImGui::Separator();
ImGui::SetNextItemWidth(200.0f);
if (ImGui::SliderFloat("Bag & Window Scale", &pendingBagScale, 0.75f, 1.5f, "%.2fx")) {
    inventoryScreen.setBagScale(pendingBagScale);
    saveCallback();
}
ImGui::SameLine();
ImGui::TextDisabled("(high-resolution default is larger)");
if (ImGui::Checkbox("Separate Bag Windows", &pendingSeparateBags)) {
    inventoryScreen.setSeparateBags(pendingSeparateBags);
    saveCallback();
}
if (ImGui::Checkbox("Show Key Ring", &pendingShowKeyring)) {
    inventoryScreen.setShowKeyring(pendingShowKeyring);
    saveCallback();
}

ImGui::Spacing();
ImGui::Separator();
ImGui::Spacing();

if (ImGui::Button("Restore Gameplay Defaults", ImVec2(-1, 0))) {
    pendingMouseSensitivity = 0.2f;
    pendingInvertMouse = false;
    pendingExtendedZoom = false;
    pendingSmoothCameraFollow = false;
    pendingUiOpacity = 65;
    pendingMinimapRotate = false;
    pendingMinimapSquare = false;
    pendingMinimapNpcDots = false;
    pendingShowMinimapClock = false;
    pendingShowMinimapCoordinates = false;
    pendingSeparateBags = true;
    inventoryScreen.setSeparateBags(true);
    pendingShowKeyring = true;
    inventoryScreen.setShowKeyring(true);
    pendingBagScale = InventoryScreen::recommendedBagScale(ImGui::GetIO().DisplaySize.y);
    inventoryScreen.setBagScale(pendingBagScale);
    pendingShowMicroMenu = true;
    uiOpacity_ = 0.65f;
    minimapRotate_ = false;
    minimapSquare_ = false;
    minimapNpcDots_ = false;
    showMinimapClock_ = false;
    showMinimapCoordinates_ = false;
    if (renderer) {
        if (auto* cameraController = renderer->getCameraController()) {
            cameraController->setMouseSensitivity(pendingMouseSensitivity);
            cameraController->setInvertMouse(pendingInvertMouse);
            cameraController->setExtendedZoom(pendingExtendedZoom);
            cameraController->setSmoothCameraFollow(pendingSmoothCameraFollow);
        }
        if (auto* minimap = renderer->getMinimap()) {
            minimap->setRotateWithCamera(minimapRotate_);
            minimap->setSquareShape(minimapSquare_);
        }
    }
    saveCallback();
}

ImGui::EndChild();

}

void SettingsPanel::renderSettingsControlsTab(std::function<void()> saveCallback) {
ImGui::Spacing();

ImGui::Text("Keybindings");
ImGui::Separator();

auto& km = ui::KeybindingManager::getInstance();
int numActions = km.getActionCount();

for (int i = 0; i < numActions; ++i) {
    auto action = static_cast<ui::KeybindingManager::Action>(i);
    const char* actionName = km.getActionName(action);
    ImGuiKey currentKey = km.getKeyForAction(action);

    // Display current binding
    ImGui::Text("%s:", actionName);
    ImGui::SameLine(200);

    // Get human-readable key name (basic implementation)
    const char* keyName = "Unknown";
    if (currentKey >= ImGuiKey_A && currentKey <= ImGuiKey_Z) {
        static char keyBuf[16];
        snprintf(keyBuf, sizeof(keyBuf), "%c", 'A' + (currentKey - ImGuiKey_A));
        keyName = keyBuf;
    } else if (currentKey >= ImGuiKey_0 && currentKey <= ImGuiKey_9) {
        static char keyBuf[16];
        snprintf(keyBuf, sizeof(keyBuf), "%c", '0' + (currentKey - ImGuiKey_0));
        keyName = keyBuf;
    } else if (currentKey == ImGuiKey_Escape) {
        keyName = "Escape";
    } else if (currentKey == ImGuiKey_Enter) {
        keyName = "Enter";
    } else if (currentKey == ImGuiKey_Tab) {
        keyName = "Tab";
    } else if (currentKey == ImGuiKey_Space) {
        keyName = "Space";
    } else if (currentKey >= ImGuiKey_F1 && currentKey <= ImGuiKey_F12) {
        static char keyBuf[16];
        snprintf(keyBuf, sizeof(keyBuf), "F%d", 1 + (currentKey - ImGuiKey_F1));
        keyName = keyBuf;
    }

    ImGui::Text("[%s]", keyName);

    // Rebind button
    ImGui::SameLine(350);
    if (ImGui::Button(awaitingKeyPress_ && pendingRebindAction_ == i ? "Waiting..." : "Rebind", ImVec2(100, 0))) {
        pendingRebindAction_ = i;
        awaitingKeyPress_ = true;
    }
}

// Handle key press during rebinding
if (awaitingKeyPress_ && pendingRebindAction_ >= 0) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Press any key to bind to this action (Esc to cancel)...");

    // Check for any key press
    bool foundKey = false;
    ImGuiKey newKey = ImGuiKey_None;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) {
            if (k == ImGuiKey_Escape) {
                // Cancel rebinding
                awaitingKeyPress_ = false;
                pendingRebindAction_ = -1;
                foundKey = true;
                break;
            }
            newKey = static_cast<ImGuiKey>(k);
            foundKey = true;
            break;
        }
    }

    if (foundKey && newKey != ImGuiKey_None) {
        auto action = static_cast<ui::KeybindingManager::Action>(pendingRebindAction_);
        km.setKeyForAction(action, newKey);
        awaitingKeyPress_ = false;
        pendingRebindAction_ = -1;
        saveCallback();
    }
}

ImGui::Spacing();
ImGui::Separator();
ImGui::Spacing();

if (ImGui::Button("Reset to Defaults", ImVec2(-1, 0))) {
    km.resetToDefaults();
    awaitingKeyPress_ = false;
    pendingRebindAction_ = -1;
    saveCallback();
}

}

void SettingsPanel::renderSettingsAudioTab(std::function<void()> saveCallback) {
    auto* renderer = services_.renderer;
ImGui::Spacing();
ImGui::BeginChild("AudioSettings", ImVec2(0, -1), true);

// Helper lambda to apply audio settings
auto applyAudioSettings = [&]() {
    applyAudioVolumes(services_.audioCoordinator);
    saveCallback();
};

ImGui::Text("Master Volume");
if (ImGui::SliderInt("##MasterVolume", &pendingMasterVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}
ImGui::Separator();

if (ImGui::Checkbox("Enable WoWee Music", &pendingUseOriginalSoundtrack)) {
    if (renderer) {
        if (auto* zm = renderer->getZoneManager()) {
            zm->setUseOriginalSoundtrack(pendingUseOriginalSoundtrack);
            if (!pendingUseOriginalSoundtrack) {
                if (auto* ac = renderer->getAudioCoordinator()) {
                    ac->onOriginalSoundtrackDisabled(zm);
                }
            }
        }
    }
    saveCallback();
}
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Include WoWee music tracks in zone music rotation");
ImGui::Separator();

ImGui::Text("Music");
if (ImGui::SliderInt("##MusicVolume", &pendingMusicVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}

ImGui::Spacing();
ImGui::Text("Ambient Sounds");
if (ImGui::SliderInt("##AmbientVolume", &pendingAmbientVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}
ImGui::TextWrapped("Weather, zones, cities, emitters");

ImGui::Spacing();
ImGui::Text("Capital City Bells");
if (ImGui::SliderInt("##BellVolume", &pendingBellVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}

ImGui::Spacing();
ImGui::Text("UI Sounds");
if (ImGui::SliderInt("##UiVolume", &pendingUiVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}
ImGui::TextWrapped("Buttons, loot, quest complete");

ImGui::Spacing();
ImGui::Text("Combat Sounds");
if (ImGui::SliderInt("##CombatVolume", &pendingCombatVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}
ImGui::TextWrapped("Weapon swings, impacts, grunts");

ImGui::Spacing();
ImGui::Text("Spell Sounds");
if (ImGui::SliderInt("##SpellVolume", &pendingSpellVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}
ImGui::TextWrapped("Magic casting and impacts");

ImGui::Spacing();
ImGui::Text("Movement Sounds");
if (ImGui::SliderInt("##MovementVolume", &pendingMovementVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}
ImGui::TextWrapped("Water splashes, jump/land");

ImGui::Spacing();
ImGui::Text("Footsteps");
if (ImGui::SliderInt("##FootstepVolume", &pendingFootstepVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}

ImGui::Spacing();
ImGui::Text("NPC Voices");
if (ImGui::SliderInt("##NpcVoiceVolume", &pendingNpcVoiceVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}

ImGui::Spacing();
if (ImGui::Checkbox("Character Speech", &pendingCharacterSpeech)) {
    applyAudioSettings();
}
if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Your character speaks error responses (\"I can't do that\", \"Not enough mana\")");

ImGui::Spacing();
ImGui::Text("Mount Sounds");
if (ImGui::SliderInt("##MountVolume", &pendingMountVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}

ImGui::Spacing();
ImGui::Text("Activity Sounds");
if (ImGui::SliderInt("##ActivityVolume", &pendingActivityVolume, 0, 100, "%d%%")) {
    applyAudioSettings();
}
ImGui::TextWrapped("Swimming, eating, drinking");

ImGui::EndChild();

if (ImGui::Button("Restore Audio Defaults", ImVec2(-1, 0))) {
    pendingMasterVolume = 100;
    pendingMusicVolume = 30; // default music volume
    pendingAmbientVolume = 100;
    pendingBellVolume = 50;
    pendingUiVolume = 100;
    pendingCombatVolume = 100;
    pendingSpellVolume = 100;
    pendingMovementVolume = 100;
    pendingFootstepVolume = 100;
    pendingNpcVoiceVolume = 100;
    pendingMountVolume = 70;
    pendingActivityVolume = 100;
    pendingCharacterSpeech = true;
    applyAudioSettings();
}

}

void SettingsPanel::renderSettingsAboutTab() {
ImGui::Spacing();
ImGui::Spacing();

ImGui::TextWrapped("WoWee - World of Warcraft Client Emulator");
ImGui::Spacing();
ImGui::Separator();
ImGui::Spacing();

ImGui::Text("Developer");
ImGui::Indent();
ImGui::Text("Kelsi Davis");
ImGui::Unindent();
ImGui::Spacing();

ImGui::Text("GitHub");
ImGui::Indent();
ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "https://github.com/Kelsidavis/WoWee");
if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip("Click to copy");
}
if (ImGui::IsItemClicked()) {
    ImGui::SetClipboardText("https://github.com/Kelsidavis/WoWee");
}
ImGui::Unindent();
ImGui::Spacing();

ImGui::Text("Contact");
ImGui::Indent();
ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "github.com/Kelsidavis");
if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip("Click to copy");
}
if (ImGui::IsItemClicked()) {
    ImGui::SetClipboardText("https://github.com/Kelsidavis");
}
ImGui::Unindent();

ImGui::Spacing();
ImGui::Separator();
ImGui::Spacing();

ImGui::TextWrapped("A multi-expansion WoW client supporting Classic, TBC, and WotLK (3.3.5a).");
ImGui::Spacing();
ImGui::TextDisabled("Built with Vulkan, SDL2, and ImGui");

}

void SettingsPanel::renderSettingsWindow(InventoryScreen& inventoryScreen, ChatPanel& chatPanel,
                                             std::function<void()> saveCallback) {
    if (!showSettingsWindow) return;

    auto* window = services_.window;
    auto* renderer = services_.renderer;
    if (!window) return;

    static constexpr int kResolutions[][2] = {
        {1280, 720},
        {1600, 900},
        {1920, 1080},
        {2560, 1440},
        {3840, 2160},
    };
    static constexpr int kResCount = sizeof(kResolutions) / sizeof(kResolutions[0]);
    constexpr int kDefaultResW = 1920;
    constexpr int kDefaultResH = 1080;
    constexpr bool kDefaultFullscreen = false;
    constexpr bool kDefaultVsync = true;
    constexpr bool kDefaultShadows = true;
    constexpr int kDefaultGroundClutterDensity = 100;

    int defaultResIndex = 0;
    for (int i = 0; i < kResCount; i++) {
        if (kResolutions[i][0] == kDefaultResW && kResolutions[i][1] == kDefaultResH) {
            defaultResIndex = i;
            break;
        }
    }

    if (!settingsInit) {
        pendingFullscreen = window->isFullscreen();
        pendingVsync = window->isVsyncEnabled();
        if (renderer) {
            renderer->setShadowsEnabled(pendingShadows);
            renderer->setShadowDistance(pendingShadowDistance);
            // Read non-volume settings from actual state (volumes come from saved settings)
            if (auto* cameraController = renderer->getCameraController()) {
                cameraController->setMouseSensitivity(pendingMouseSensitivity);
                cameraController->setInvertMouse(pendingInvertMouse);
                cameraController->setExtendedZoom(pendingExtendedZoom);
                cameraController->setCameraSmoothSpeed(pendingCameraStiffness);
                cameraController->setPivotHeight(pendingPivotHeight);
                cameraController->setIdleOrbitEnabled(pendingIdleCameraOrbit);
                cameraController->setSmoothCameraFollow(pendingSmoothCameraFollow);
            }
        }
        pendingResIndex = 0;
        int curW = window->getWidth();
        int curH = window->getHeight();
        if (!displaySettingsLoaded_) {
            pendingResolutionWidth = curW;
            pendingResolutionHeight = curH;
        }
        long long bestDistance = std::numeric_limits<long long>::max();
        for (int i = 0; i < kResCount; i++) {
            const long long dx = static_cast<long long>(kResolutions[i][0]) - pendingResolutionWidth;
            const long long dy = static_cast<long long>(kResolutions[i][1]) - pendingResolutionHeight;
            const long long distance = dx * dx + dy * dy;
            if (distance < bestDistance) {
                bestDistance = distance;
                pendingResIndex = i;
            }
        }
        pendingUiOpacity = static_cast<int>(std::lround(uiOpacity_ * 100.0f));
        pendingMinimapRotate = minimapRotate_;
        pendingMinimapSquare = minimapSquare_;
        pendingMinimapNpcDots = minimapNpcDots_;
        pendingShowMinimapClock = showMinimapClock_;
        pendingShowMinimapCoordinates = showMinimapCoordinates_;
        pendingShowLatencyMeter = showLatencyMeter_;
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                minimap->setRotateWithCamera(minimapRotate_);
                minimap->setSquareShape(minimapSquare_);
            }
            if (auto* zm = renderer->getZoneManager()) {
                pendingUseOriginalSoundtrack = zm->getUseOriginalSoundtrack();
            }
        }
        settingsInit = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;
    // Give the settings surface enough room on high-resolution displays while
    // retaining a sensible minimum for 1080p and laptop screens.
    ImVec2 size(std::clamp(650.0f * appliedWindowUiScale_, 520.0f, screenW * 0.90f),
                std::clamp(std::min(screenH * 0.90f, 900.0f * appliedWindowUiScale_), 560.0f, screenH * 0.90f));
    ImVec2 pos((screenW - size.x) * 0.5f, (screenH - size.y) * 0.5f);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;

    if (ImGui::Begin("##SettingsWindow", nullptr, flags)) {
        ImGui::Text("Settings");
        ImGui::SameLine();
        {
            // Right-align the build version against the window's content edge.
            const char* version = core::kVersionString;
            float versionWidth = ImGui::CalcTextSize(version).x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - versionWidth);
            ImGui::TextDisabled("%s", version);
        }
        ImGui::Separator();

        // Keep the action row outside the scrolling tab region so it remains
        // visible regardless of which tab or section is active.
        const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 18.0f;
        ImGui::BeginChild("SettingsTabRegion", ImVec2(0, -footerHeight), false);
        if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {
            // ============================================================
            // VIDEO TAB
            // ============================================================
            if (ImGui::BeginTabItem("Video")) {
                ImGui::Spacing();

                // Graphics Quality Presets
                {
                    const char* presetLabels[] = { "Custom", "Low", "Medium", "High", "Ultra" };
                    int presetIdx = static_cast<int>(pendingGraphicsPreset);
                    if (ImGui::Combo("Quality Preset", &presetIdx, presetLabels, 5)) {
                        pendingGraphicsPreset = static_cast<GraphicsPreset>(presetIdx);
                        if (pendingGraphicsPreset != GraphicsPreset::CUSTOM) {
                            applyGraphicsPreset(pendingGraphicsPreset);
                            saveCallback();
                        }
                    }
                    ImGui::TextDisabled("Adjust these for custom settings");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Checkbox("Fullscreen", &pendingFullscreen)) {
                    window->setFullscreen(pendingFullscreen);
                    if (pendingFullscreen) {
                        window->applyResolution(pendingResolutionWidth, pendingResolutionHeight);
                    }
                    updateGraphicsPresetFromCurrentSettings();
                    saveCallback();
                }
                if (ImGui::Checkbox("VSync", &pendingVsync)) {
                    window->setVsync(pendingVsync);
                    updateGraphicsPresetFromCurrentSettings();
                    saveCallback();
                }
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::SliderFloat("View Distance", &pendingViewDistance,
                                       400.0f, 2400.0f, "%.0f")) {
                    if (renderer) renderer->setViewDistance(pendingViewDistance);
                    updateGraphicsPresetFromCurrentSettings();
                    saveCallback();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Controls terrain, world-object, and doodad draw distance.");
                }
                if (ImGui::Checkbox("Shadows", &pendingShadows)) {
                    if (renderer) renderer->setShadowsEnabled(pendingShadows);
                    updateGraphicsPresetFromCurrentSettings();
                    saveCallback();
                }
                if (pendingShadows) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::SliderFloat("Distance##shadow", &pendingShadowDistance, 40.0f, 500.0f, "%.0f")) {
                        if (renderer) renderer->setShadowDistance(pendingShadowDistance);
                        updateGraphicsPresetFromCurrentSettings();
                        saveCallback();
                    }
                }
                {
                    if (ImGui::Checkbox("Water Refraction", &pendingWaterRefraction)) {
                        if (renderer) renderer->setWaterRefractionEnabled(pendingWaterRefraction);
                        updateGraphicsPresetFromCurrentSettings();
                        saveCallback();
                    }
                }
                {
                    const char* aaLabels[] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
                    bool fsr2Active = renderer && renderer->getPostProcessPipeline()->isFSR2Enabled();
                    if (fsr2Active) {
                        ImGui::BeginDisabled();
                        int disabled = 0;
                        ImGui::Combo("Anti-Aliasing (FSR3)", &disabled, "Off (FSR3 active)\0", 1);
                        ImGui::EndDisabled();
                    } else if (ImGui::Combo("Anti-Aliasing", &pendingAntiAliasing, aaLabels, 4)) {
                        static const VkSampleCountFlagBits aaSamples[] = {
                            VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT,
                            VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT
                        };
                        if (renderer) renderer->setMsaaSamples(aaSamples[pendingAntiAliasing]);
                        updateGraphicsPresetFromCurrentSettings();
                        saveCallback();
                    }
                    // FXAA — post-process, combinable with MSAA or FSR3
                    {
                        if (ImGui::Checkbox("FXAA (post-process)", &pendingFXAA)) {
                            if (renderer) renderer->getPostProcessPipeline()->setFXAAEnabled(pendingFXAA);
                            updateGraphicsPresetFromCurrentSettings();
                            saveCallback();
                        }
                        if (ImGui::IsItemHovered()) {
                            if (fsr2Active)
                                ImGui::SetTooltip("FXAA applies spatial anti-aliasing after FSR3 upscaling.\nFSR3 + FXAA is the recommended ultra-quality combination.");
                            else
                                ImGui::SetTooltip("FXAA smooths jagged edges as a post-process pass.\nCan be combined with MSAA for extra quality.");
                        }
                    }
                }
                // AMD FidelityFX is not exposed on macOS/MoltenVK. Keep the
                // controls and experimental frame-generation path off that
                // platform rather than advertising unsupported settings.
#ifndef __APPLE__
                // FSR Upscaling
                {
                    // FSR mode selection: Off, FSR 1.0 (Spatial), FSR 3.x (Temporal)
                    const char* fsrModeLabels[] = { "Off", "FSR 1.0 (Spatial)", "FSR 3.x (Temporal)" };
                    int fsrMode = pendingUpscalingMode;
                    if (ImGui::Combo("Upscaling", &fsrMode, fsrModeLabels, 3)) {
                        pendingUpscalingMode = fsrMode;
                        pendingFSR = (fsrMode == 1);
                        if (renderer) {
                            renderer->setFSREnabled(fsrMode == 1);
                            renderer->setFSR2Enabled(fsrMode == 2);
                        }
                        saveCallback();
                    }
                    if (fsrMode > 0) {
                        if (fsrMode == 2 && renderer) {
                            ImGui::TextDisabled("FSR3 backend: %s",
                                renderer->getPostProcessPipeline()->isAmdFsr2SdkAvailable() ? "AMD FidelityFX SDK" : "Internal fallback");
                            if (renderer->getPostProcessPipeline()->isAmdFsr3FramegenSdkAvailable()) {
                                if (ImGui::Checkbox("AMD FSR3 Frame Generation (Experimental)", &pendingAMDFramegen)) {
                                    renderer->getPostProcessPipeline()->setAmdFsr3FramegenEnabled(pendingAMDFramegen);
                                    saveCallback();
                                }
                                const char* runtimeStatus = "Unavailable";
                                if (renderer->getPostProcessPipeline()->isAmdFsr3FramegenRuntimeActive()) {
                                    runtimeStatus = "Active";
                                } else if (renderer->getPostProcessPipeline()->isAmdFsr3FramegenRuntimeReady()) {
                                    runtimeStatus = "Ready";
                                } else {
                                    runtimeStatus = "Unavailable";
                                }
                                ImGui::TextDisabled("Runtime: %s (%s)",
                                    runtimeStatus, renderer->getPostProcessPipeline()->getAmdFsr3FramegenRuntimePath());
                                if (!renderer->getPostProcessPipeline()->isAmdFsr3FramegenRuntimeReady()) {
                                    const std::string& runtimeErr = renderer->getPostProcessPipeline()->getAmdFsr3FramegenRuntimeError();
                                    if (!runtimeErr.empty()) {
                                        ImGui::TextDisabled("Reason: %s", runtimeErr.c_str());
                                    }
                                }
                            } else {
                                ImGui::BeginDisabled();
                                bool disabledFg = false;
                                ImGui::Checkbox("AMD FSR3 Frame Generation (Experimental)", &disabledFg);
                                ImGui::EndDisabled();
                                ImGui::TextDisabled("Requires FidelityFX-SDK framegen headers.");
                            }
                        }
                        const char* fsrQualityLabels[] = { "Native (100%)", "Ultra Quality (77%)", "Quality (67%)", "Balanced (59%)" };
                        static constexpr float fsrScaleFactors[] = { 0.77f, 0.67f, 0.59f, 1.00f };
                        static constexpr int displayToInternal[] = { 3, 0, 1, 2 };
                        pendingFSRQuality = std::clamp(pendingFSRQuality, 0, 3);
                        int fsrQualityDisplay = 0;
                        for (int i = 0; i < 4; ++i) {
                            if (displayToInternal[i] == pendingFSRQuality) {
                                fsrQualityDisplay = i;
                                break;
                            }
                        }
                        if (ImGui::Combo("FSR Quality", &fsrQualityDisplay, fsrQualityLabels, 4)) {
                            pendingFSRQuality = displayToInternal[fsrQualityDisplay];
                            if (renderer) renderer->getPostProcessPipeline()->setFSRQuality(fsrScaleFactors[pendingFSRQuality]);
                            saveCallback();
                        }
                        if (ImGui::SliderFloat("FSR Sharpness", &pendingFSRSharpness, 0.0f, 2.0f, "%.1f")) {
                            if (renderer) renderer->getPostProcessPipeline()->setFSRSharpness(pendingFSRSharpness);
                            saveCallback();
                        }
                        if (fsrMode == 2) {
                            ImGui::SeparatorText("FSR3 Tuning");
                            if (ImGui::SliderFloat("Jitter Sign", &pendingFSR2JitterSign, -2.0f, 2.0f, "%.2f")) {
                                if (renderer) {
                                    renderer->getPostProcessPipeline()->setFSR2DebugTuning(
                                        pendingFSR2JitterSign,
                                        pendingFSR2MotionVecScaleX,
                                        pendingFSR2MotionVecScaleY);
                                }
                                saveCallback();
                            }
                            ImGui::TextDisabled("Tip: 0.38 is the current recommended default.");
                        }
                    }
                }
#endif
                if (ImGui::SliderInt("Ground Clutter Density", &pendingGroundClutterDensity, 0, 150, "%d%%")) {
                    if (renderer) {
                        if (auto* tm = renderer->getTerrainManager()) {
                            tm->setGroundClutterDensityScale(static_cast<float>(pendingGroundClutterDensity) / 100.0f);
                        }
                    }
                    saveCallback();
                }
                if (ImGui::Checkbox("Normal Mapping", &pendingNormalMapping)) {
                    if (renderer) {
                        if (auto* wr = renderer->getWMORenderer()) {
                            wr->setNormalMappingEnabled(pendingNormalMapping);
                        }
                        if (auto* cr = renderer->getCharacterRenderer()) {
                            cr->setNormalMappingEnabled(pendingNormalMapping);
                        }
                    }
                    saveCallback();
                }
                if (pendingNormalMapping) {
                    if (ImGui::SliderFloat("Normal Map Strength", &pendingNormalMapStrength, 0.0f, 2.0f, "%.1f")) {
                        if (renderer) {
                            if (auto* wr = renderer->getWMORenderer()) {
                                wr->setNormalMapStrength(pendingNormalMapStrength);
                            }
                            if (auto* cr = renderer->getCharacterRenderer()) {
                                cr->setNormalMapStrength(pendingNormalMapStrength);
                            }
                        }
                        saveCallback();
                    }
                }
                if (ImGui::Checkbox("Parallax Mapping", &pendingPOM)) {
                    if (renderer) {
                        if (auto* wr = renderer->getWMORenderer()) {
                            wr->setPOMEnabled(pendingPOM);
                        }
                        if (auto* cr = renderer->getCharacterRenderer()) {
                            cr->setPOMEnabled(pendingPOM);
                        }
                    }
                    saveCallback();
                }
                if (pendingPOM) {
                    const char* pomLabels[] = { "Low", "Medium", "High" };
                    if (ImGui::Combo("Parallax Quality", &pendingPOMQuality, pomLabels, 3)) {
                        if (renderer) {
                            if (auto* wr = renderer->getWMORenderer()) {
                                wr->setPOMQuality(pendingPOMQuality);
                            }
                            if (auto* cr = renderer->getCharacterRenderer()) {
                                cr->setPOMQuality(pendingPOMQuality);
                            }
                        }
                        saveCallback();
                    }
                }

                const char* resLabel = "Resolution";
                const char* resItems[kResCount];
                char resBuf[kResCount][16];
                for (int i = 0; i < kResCount; i++) {
                    snprintf(resBuf[i], sizeof(resBuf[i]), "%dx%d", kResolutions[i][0], kResolutions[i][1]);
                    resItems[i] = resBuf[i];
                }
                if (ImGui::Combo(resLabel, &pendingResIndex, resItems, kResCount)) {
                    pendingResolutionWidth = kResolutions[pendingResIndex][0];
                    pendingResolutionHeight = kResolutions[pendingResIndex][1];
                    window->applyResolution(pendingResolutionWidth, pendingResolutionHeight);
                    saveCallback();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::SliderInt("Brightness", &pendingBrightness, 0, 100, "%d%%")) {
                    if (renderer) renderer->getPostProcessPipeline()->setBrightness(static_cast<float>(pendingBrightness) / 50.0f);
                    saveCallback();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Restore Video Defaults", ImVec2(-1, 0))) {
                    pendingFullscreen = kDefaultFullscreen;
                    pendingVsync = kDefaultVsync;
                    pendingShadows = kDefaultShadows;
                    pendingShadowDistance = 300.0f;
                    pendingGroundClutterDensity = kDefaultGroundClutterDensity;
                    pendingAntiAliasing = 0;
                    pendingNormalMapping = true;
                    pendingNormalMapStrength = 0.8f;
                    pendingPOM = true;
                    pendingPOMQuality = 1;
                    pendingResIndex = defaultResIndex;
                    pendingResolutionWidth = kDefaultResW;
                    pendingResolutionHeight = kDefaultResH;
                    pendingBrightness = 50;
                    window->setFullscreen(pendingFullscreen);
                    window->setVsync(pendingVsync);
                    window->applyResolution(pendingResolutionWidth, pendingResolutionHeight);
                    if (renderer) renderer->getPostProcessPipeline()->setBrightness(1.0f);
                    pendingWaterRefraction = false;
                    if (renderer) {
                        renderer->setShadowsEnabled(pendingShadows);
                        renderer->setShadowDistance(pendingShadowDistance);
                    }
                    if (renderer) renderer->setWaterRefractionEnabled(pendingWaterRefraction);
                    if (renderer) renderer->setMsaaSamples(VK_SAMPLE_COUNT_1_BIT);
                    if (renderer) {
                        if (auto* tm = renderer->getTerrainManager()) {
                            tm->setGroundClutterDensityScale(static_cast<float>(pendingGroundClutterDensity) / 100.0f);
                        }
                    }
                    if (renderer) {
                        if (auto* wr = renderer->getWMORenderer()) {
                            wr->setNormalMappingEnabled(pendingNormalMapping);
                            wr->setNormalMapStrength(pendingNormalMapStrength);
                            wr->setPOMEnabled(pendingPOM);
                            wr->setPOMQuality(pendingPOMQuality);
                        }
                        if (auto* cr = renderer->getCharacterRenderer()) {
                            cr->setNormalMappingEnabled(pendingNormalMapping);
                            cr->setNormalMapStrength(pendingNormalMapStrength);
                            cr->setPOMEnabled(pendingPOM);
                            cr->setPOMQuality(pendingPOMQuality);
                        }
                    }
                    saveCallback();
                }

                ImGui::EndTabItem();
            }

            // ============================================================
            // INTERFACE TAB
            // ============================================================
            if (ImGui::BeginTabItem("Interface")) {
                renderSettingsInterfaceTab(saveCallback);
                ImGui::EndTabItem();
            }

            // ============================================================
            // AUDIO TAB
            // ============================================================
            if (ImGui::BeginTabItem("Audio")) {
                renderSettingsAudioTab(saveCallback);
                ImGui::EndTabItem();
            }

            // ============================================================
            // GAMEPLAY TAB
            // ============================================================
            if (ImGui::BeginTabItem("Gameplay")) {
                renderSettingsGameplayTab(inventoryScreen, saveCallback);
                ImGui::EndTabItem();
            }

            // ============================================================
            // CONTROLS TAB
            // ============================================================
            if (ImGui::BeginTabItem("Controls")) {
                renderSettingsControlsTab(saveCallback);
                ImGui::EndTabItem();
            }

            // ============================================================
            // CHAT TAB
            // ============================================================
            if (ImGui::BeginTabItem("Chat")) {
                chatPanel.renderSettingsTab(saveCallback);
                ImGui::EndTabItem();
            }

            // ============================================================
            // ABOUT TAB
            // ============================================================
            if (ImGui::BeginTabItem("About")) {
                renderSettingsAboutTab();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 10.0f));
        float saveBtnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Save Settings", ImVec2(saveBtnW, 0))) {
            saveCallback();
        }
        ImGui::SameLine();
        if (ImGui::Button("Back to Game", ImVec2(-1, 0))) {
            showSettingsWindow = false;
        }
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

void SettingsPanel::applyWindowUiScale() {
    if (!ImGui::GetCurrentContext()) return;

    pendingWindowUiScale = std::clamp(pendingWindowUiScale, 0.75f, 1.5f);
    if (windowUiScaleEditing_) return;
    if (std::abs(appliedWindowUiScale_ - pendingWindowUiScale) < 0.0001f) {
        ImGui::GetIO().FontGlobalScale = pendingWindowUiScale;
        return;
    }

    // Scale from the currently applied value, not from the already-scaled
    // style, so dragging the slider back and forth never compounds rounding.
    const float ratio = pendingWindowUiScale / appliedWindowUiScale_;
    ImGui::GetStyle().ScaleAllSizes(ratio);
    ImGui::GetIO().FontGlobalScale = pendingWindowUiScale;
    appliedWindowUiScale_ = pendingWindowUiScale;
}

void SettingsPanel::applyGraphicsPreset(GraphicsPreset preset) {
    auto* renderer = services_.renderer;

    // Define preset values based on quality level
    switch (preset) {
        case GraphicsPreset::LOW: {
            pendingViewDistance = 600.0f;
            pendingShadows = false;
            pendingShadowDistance = 100.0f;
            pendingAntiAliasing = 0;  // Off
            pendingNormalMapping = false;
            pendingPOM = false;
            pendingGroundClutterDensity = 25;
            if (renderer) {
                renderer->setViewDistance(pendingViewDistance);
                renderer->setShadowsEnabled(false);
                renderer->setMsaaSamples(VK_SAMPLE_COUNT_1_BIT);
                if (auto* wr = renderer->getWMORenderer()) {
                    wr->setNormalMappingEnabled(false);
                    wr->setPOMEnabled(false);
                }
                if (auto* cr = renderer->getCharacterRenderer()) {
                    cr->setNormalMappingEnabled(false);
                    cr->setPOMEnabled(false);
                }
                if (auto* tm = renderer->getTerrainManager()) {
                    tm->setGroundClutterDensityScale(0.25f);
                }
            }
            break;
        }
        case GraphicsPreset::MEDIUM: {
            pendingViewDistance = 1000.0f;
            pendingShadows = true;
            pendingShadowDistance = 200.0f;
            pendingAntiAliasing = 1;  // 2x MSAA
            pendingNormalMapping = true;
            pendingNormalMapStrength = 0.6f;
            pendingPOM = true;
            pendingPOMQuality = 0;  // Low
            pendingGroundClutterDensity = 60;
            if (renderer) {
                renderer->setViewDistance(pendingViewDistance);
                renderer->setShadowsEnabled(true);
                renderer->setShadowDistance(200.0f);
                renderer->setMsaaSamples(VK_SAMPLE_COUNT_2_BIT);
                if (auto* wr = renderer->getWMORenderer()) {
                    wr->setNormalMappingEnabled(true);
                    wr->setNormalMapStrength(0.6f);
                    wr->setPOMEnabled(true);
                    wr->setPOMQuality(0);
                }
                if (auto* cr = renderer->getCharacterRenderer()) {
                    cr->setNormalMappingEnabled(true);
                    cr->setNormalMapStrength(0.6f);
                    cr->setPOMEnabled(true);
                    cr->setPOMQuality(0);
                }
                if (auto* tm = renderer->getTerrainManager()) {
                    tm->setGroundClutterDensityScale(0.60f);
                }
            }
            break;
        }
        case GraphicsPreset::HIGH: {
            pendingViewDistance = 1600.0f;
            pendingShadows = true;
            pendingShadowDistance = 350.0f;
            pendingAntiAliasing = 2;  // 4x MSAA
            pendingNormalMapping = true;
            pendingNormalMapStrength = 0.8f;
            pendingPOM = true;
            pendingPOMQuality = 1;  // Medium
            pendingGroundClutterDensity = 100;
            if (renderer) {
                renderer->setViewDistance(pendingViewDistance);
                renderer->setShadowsEnabled(true);
                renderer->setShadowDistance(350.0f);
                renderer->setMsaaSamples(VK_SAMPLE_COUNT_4_BIT);
                if (auto* wr = renderer->getWMORenderer()) {
                    wr->setNormalMappingEnabled(true);
                    wr->setNormalMapStrength(0.8f);
                    wr->setPOMEnabled(true);
                    wr->setPOMQuality(1);
                }
                if (auto* cr = renderer->getCharacterRenderer()) {
                    cr->setNormalMappingEnabled(true);
                    cr->setNormalMapStrength(0.8f);
                    cr->setPOMEnabled(true);
                    cr->setPOMQuality(1);
                }
                if (auto* tm = renderer->getTerrainManager()) {
                    tm->setGroundClutterDensityScale(1.0f);
                }
            }
            break;
        }
        case GraphicsPreset::ULTRA: {
            pendingViewDistance = 2400.0f;
            pendingShadows = true;
            pendingShadowDistance = 500.0f;
            pendingAntiAliasing = 3;  // 8x MSAA
            pendingFXAA = true;       // FXAA on top of MSAA for maximum smoothness
            pendingNormalMapping = true;
            pendingNormalMapStrength = 1.2f;
            pendingPOM = true;
            pendingPOMQuality = 2;  // High
            pendingGroundClutterDensity = 150;
            if (renderer) {
                renderer->setViewDistance(pendingViewDistance);
                renderer->setShadowsEnabled(true);
                renderer->setShadowDistance(500.0f);
                renderer->setMsaaSamples(VK_SAMPLE_COUNT_8_BIT);
                renderer->getPostProcessPipeline()->setFXAAEnabled(true);
                if (auto* wr = renderer->getWMORenderer()) {
                    wr->setNormalMappingEnabled(true);
                    wr->setNormalMapStrength(1.2f);
                    wr->setPOMEnabled(true);
                    wr->setPOMQuality(2);
                }
                if (auto* cr = renderer->getCharacterRenderer()) {
                    cr->setNormalMappingEnabled(true);
                    cr->setNormalMapStrength(1.2f);
                    cr->setPOMEnabled(true);
                    cr->setPOMQuality(2);
                }
                if (auto* tm = renderer->getTerrainManager()) {
                    tm->setGroundClutterDensityScale(1.5f);
                }
            }
            break;
        }
        default:
            break;
    }

    currentGraphicsPreset = preset;
    pendingGraphicsPreset = preset;
}

void SettingsPanel::updateGraphicsPresetFromCurrentSettings() {
    // Check if current settings match any preset, otherwise mark as CUSTOM
    // This is a simplified check; could be enhanced with more detailed matching

    auto matchesPreset = [this](GraphicsPreset preset) -> bool {
        switch (preset) {
            case GraphicsPreset::LOW:
                return pendingViewDistance >= 580.0f && pendingViewDistance <= 620.0f &&
                       !pendingShadows && pendingAntiAliasing == 0 && !pendingNormalMapping && !pendingPOM &&
                       pendingGroundClutterDensity <= 30;
            case GraphicsPreset::MEDIUM:
                return pendingViewDistance >= 980.0f && pendingViewDistance <= 1020.0f &&
                       pendingShadows && pendingShadowDistance >= 180 && pendingShadowDistance <= 220 &&
                       pendingAntiAliasing == 1 && pendingNormalMapping && pendingPOM &&
                       pendingGroundClutterDensity >= 50 && pendingGroundClutterDensity <= 70;
            case GraphicsPreset::HIGH:
                return pendingViewDistance >= 1580.0f && pendingViewDistance <= 1620.0f &&
                       pendingShadows && pendingShadowDistance >= 330 && pendingShadowDistance <= 370 &&
                       pendingAntiAliasing == 2 && pendingNormalMapping && pendingPOM &&
                       pendingGroundClutterDensity >= 90 && pendingGroundClutterDensity <= 110;
            case GraphicsPreset::ULTRA:
                return pendingViewDistance >= 2380.0f && pendingShadows &&
                       pendingShadowDistance >= 480 && pendingAntiAliasing == 3 &&
                       pendingFXAA && pendingNormalMapping && pendingPOM && pendingGroundClutterDensity >= 140;
            default:
                return false;
        }
    };

    // Try to match a preset, otherwise mark as custom
    if (matchesPreset(GraphicsPreset::LOW)) {
        pendingGraphicsPreset = GraphicsPreset::LOW;
    } else if (matchesPreset(GraphicsPreset::MEDIUM)) {
        pendingGraphicsPreset = GraphicsPreset::MEDIUM;
    } else if (matchesPreset(GraphicsPreset::HIGH)) {
        pendingGraphicsPreset = GraphicsPreset::HIGH;
    } else if (matchesPreset(GraphicsPreset::ULTRA)) {
        pendingGraphicsPreset = GraphicsPreset::ULTRA;
    } else {
        pendingGraphicsPreset = GraphicsPreset::CUSTOM;
    }
}

std::string SettingsPanel::getSettingsPath() {
    return core::getConfigRoot() + "/settings.cfg";
}

void SettingsPanel::applyAudioVolumes(audio::AudioCoordinator* ac) {
    if (!ac) return;
    float masterScale = soundMuted_ ? 0.0f : static_cast<float>(pendingMasterVolume) / 100.0f;
    audio::AudioEngine::instance().setMasterVolume(masterScale);
    if (auto* music = ac->getMusicManager())
        music->setVolume(pendingMusicVolume);
    if (auto* ambient = ac->getAmbientSoundManager())
    {
        ambient->setVolumeScale(pendingAmbientVolume / 100.0f);
        ambient->setBellVolumeScale(pendingBellVolume / 100.0f);
    }
    if (auto* ui = ac->getUiSoundManager())
        ui->setVolumeScale(pendingUiVolume / 100.0f);
    if (auto* combat = ac->getCombatSoundManager())
        combat->setVolumeScale(pendingCombatVolume / 100.0f);
    if (auto* spell = ac->getSpellSoundManager())
        spell->setVolumeScale(pendingSpellVolume / 100.0f);
    if (auto* movement = ac->getMovementSoundManager())
        movement->setVolumeScale(pendingMovementVolume / 100.0f);
    if (auto* footstep = ac->getFootstepManager())
        footstep->setVolumeScale(pendingFootstepVolume / 100.0f);
    if (auto* npcVoice = ac->getNpcVoiceManager())
        npcVoice->setVolumeScale(pendingNpcVoiceVolume / 100.0f);
    if (auto* playerVoice = ac->getPlayerVoiceManager())
        playerVoice->setEnabled(pendingCharacterSpeech);
    if (auto* mount = ac->getMountSoundManager())
        mount->setVolumeScale(pendingMountVolume / 100.0f);
    if (auto* activity = ac->getActivitySoundManager())
        activity->setVolumeScale(pendingActivityVolume / 100.0f);
}


} // namespace ui
} // namespace wowee
