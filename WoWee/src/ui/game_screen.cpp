#include "ui/game_screen.hpp"
#include "ui/scene_pick.hpp"
#include "ui/ui_colors.hpp"
#include "ui/ui_helpers.hpp"
#include "rendering/vk_context.hpp"
#include "core/application.hpp"
#include "core/appearance_composer.hpp"
#include "addons/addon_manager.hpp"
#include "core/coordinates.hpp"
#include "core/input.hpp"
#include "rendering/renderer.hpp"
#include "rendering/post_process_pipeline.hpp"
#include "rendering/animation_controller.hpp"
#include "rendering/wmo_renderer.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/minimap.hpp"
#include "rendering/world_map.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "game/zone_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/spell_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"

#include "game/expansion_profile.hpp"
#include "game/character.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <chrono>
#include <ctime>

#include <unordered_set>

namespace {
    using namespace wowee::ui::colors;
    using namespace wowee::ui::helpers;
    constexpr auto& kColorRed        = kRed;
    constexpr auto& kColorBrightGreen= kBrightGreen;
    constexpr auto& kColorYellow     = kYellow;

    // Upper bound on a game object's cursor-pick sphere. A wide/tall M2 GO (a forge, an
    // anvil, a large brazier) reports a legitimately big visual radius, but its bounding
    // sphere is a poor click target: the half-diagonal reaches well past the geometry and
    // swallows NPCs standing next to it, so those units become unclickable. Clamping the
    // GO pick radius keeps large objects clickable near their center without stealing the
    // click from a neighbor. Units are never clamped — this is a GO-only correction, the
    // same reasoning that already makes WMO GOs fall back to a conservative fixed sphere.

    // True when the client's own 1.12 FrameXML is loaded and drawn. Its
    // PlayerFrame, MainMenuBar, bag slots, micro buttons, XP bar and
    // QuestWatchFrame then cover the same ground as these ImGui panels, and
    // drawing both stacks two interfaces on top of each other.
    bool originalInterfaceActive() {
        static const bool on = [] {
            const auto set = [](const char* n) {
                const char* v = std::getenv(n);
                return v && *v && std::string(v) != "0";
            };
            return set("WOWEE_LOAD_FRAMEXML") && set("WOWEE_FRAMEXML_UI");
        }();
        return on;
    }

    bool raySphereIntersect(const wowee::rendering::Ray& ray, const glm::vec3& center, float radius, float& tOut) {
        glm::vec3 oc = ray.origin - center;
        float b = glm::dot(oc, ray.direction);
        float c = glm::dot(oc, oc) - radius * radius;
        float discriminant = b * b - c;
        if (discriminant < 0.0f) return false;
        float t = -b - std::sqrt(discriminant);
        if (t < 0.0f) t = -b + std::sqrt(discriminant);
        if (t < 0.0f) return false;
        tOut = t;
        return true;
    }

    std::string getEntityName(const std::shared_ptr<wowee::game::Entity>& entity) {
        if (entity->getType() == wowee::game::ObjectType::PLAYER) {
            auto player = std::static_pointer_cast<wowee::game::Player>(entity);
            if (!player->getName().empty()) return player->getName();
        } else if (entity->getType() == wowee::game::ObjectType::UNIT) {
            auto unit = std::static_pointer_cast<wowee::game::Unit>(entity);
            if (!unit->getName().empty()) return unit->getName();
        } else if (entity->getType() == wowee::game::ObjectType::GAMEOBJECT) {
            auto go = std::static_pointer_cast<wowee::game::GameObject>(entity);
            if (!go->getName().empty()) return go->getName();
        }
        return "Unknown";
    }

    // Draw a four-edge screen vignette (gradient overlay along each edge).
    // Used for damage flash, low-health pulse, and level-up golden burst.
    void drawScreenEdgeVignette(uint8_t r, uint8_t g, uint8_t b,
                                int alpha, float thicknessRatio) {
        if (alpha <= 0) return;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float W = ImGui::GetIO().DisplaySize.x;
        const float H = ImGui::GetIO().DisplaySize.y;
        const float thickness = std::min(W, H) * thicknessRatio;
        const ImU32 edgeCol = IM_COL32(r, g, b, alpha);
        const ImU32 fadeCol = IM_COL32(r, g, b, 0);
        // Top
        fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, thickness),
                                    edgeCol, edgeCol, fadeCol, fadeCol);
        // Bottom
        fg->AddRectFilledMultiColor(ImVec2(0, H - thickness), ImVec2(W, H),
                                    fadeCol, fadeCol, edgeCol, edgeCol);
        // Left
        fg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(thickness, H),
                                    edgeCol, fadeCol, fadeCol, edgeCol);
        // Right
        fg->AddRectFilledMultiColor(ImVec2(W - thickness, 0), ImVec2(W, H),
                                    fadeCol, edgeCol, edgeCol, fadeCol);
    }

}

namespace wowee { namespace ui {

GameScreen::GameScreen() {
    loadSettings();
}

// Set UI services and propagate to child components

namespace {
/// How far the cursor may travel between press and release and still count as a
/// click rather than a camera drag.
///
/// This was a flat five pixels, which is a different gesture on different
/// screens: five pixels is a third of a percent of a 1280-wide window and a
/// tenth of that on a 3840-wide one, so the same small hand movement that reads
/// as a click on a modest display reads as a drag on a large one — and a
/// discarded right-click is an NPC that cannot be interacted with at all.
/// Scaled by the window, with a floor so it never becomes stricter than it was.
float clickDragThreshold() {
    const ImGuiIO& io = ImGui::GetIO();
    const float shorter = std::min(io.DisplaySize.x, io.DisplaySize.y);
    return std::max(5.0f, shorter * 0.008f);
}
}

void GameScreen::setServices(const UIServices& services) {
    services_ = services;
    // Settings are loaded by the constructor before services are injected.
    // Apply the saved display pacing as soon as the actual window is available.
    if (services_.window &&
        services_.window->isVsyncEnabled() != settingsPanel_.pendingVsync) {
        services_.window->setVsync(settingsPanel_.pendingVsync);
    }
    if (services_.window && settingsPanel_.displaySettingsLoaded_) {
        services_.window->setFullscreen(settingsPanel_.pendingFullscreen);
        services_.window->applyResolution(settingsPanel_.pendingResolutionWidth,
                                          settingsPanel_.pendingResolutionHeight);
    }
    // Update legacy pointer for compatibility
    appearanceComposer_ = services.appearanceComposer;
    // Propagate to child panels
    chatPanel_.setServices(services);
    toastManager_.setServices(services);
    dialogManager_.setServices(services);
    settingsPanel_.setServices(services);
    combatUI_.setServices(services);
    socialPanel_.setServices(services);
    actionBarPanel_.setServices(services);
    windowManager_.setServices(services);
    applyCameraControlSettings();
    applyStartupGraphicsAndAudio();
}

void GameScreen::applyCameraControlSettings() {
    auto* renderer = services_.renderer;
    if (!renderer) return;

    if (auto* cam = renderer->getCameraController()) {
        cam->setMouseSensitivity(settingsPanel_.pendingMouseSensitivity);
        cam->setInvertMouse(settingsPanel_.pendingInvertMouse);
        cam->setRelativeMouseEnabled(settingsPanel_.pendingRelativeMouse);
        cam->setExtendedZoom(settingsPanel_.pendingExtendedZoom);
        cam->setCameraSmoothSpeed(settingsPanel_.pendingCameraStiffness);
        cam->setPivotHeight(settingsPanel_.pendingPivotHeight);
        cam->setIdleOrbitEnabled(settingsPanel_.pendingIdleCameraOrbit);
        cam->setSmoothCameraFollow(settingsPanel_.pendingSmoothCameraFollow);
    }
}

void GameScreen::applyStartupGraphicsAndAudio() {
    auto* renderer = services_.renderer;
    if (renderer && !settingsPanel_.lightingSettingsApplied_) {
        renderer->setShadowsEnabled(settingsPanel_.pendingShadows);
        renderer->setShadowDistance(settingsPanel_.pendingShadowDistance);
        renderer->setViewDistance(settingsPanel_.pendingViewDistance);
        if (auto* post = renderer->getPostProcessPipeline()) {
            post->setBrightness(static_cast<float>(settingsPanel_.pendingBrightness) / 50.0f);
        }
        settingsPanel_.lightingSettingsApplied_ = true;
        LOG_INFO("Applied startup graphics from settings.cfg (shadows=",
                 settingsPanel_.pendingShadows ? 1 : 0,
                 ", viewDistance=", settingsPanel_.pendingViewDistance, ")");
    }

    if (!settingsPanel_.volumeSettingsApplied_) {
        auto* ac = services_.audioCoordinator;
        if (ac && ac->getUiSoundManager()) {
            settingsPanel_.applyAudioVolumes(ac);
            settingsPanel_.volumeSettingsApplied_ = true;
            LOG_INFO("Applied startup audio volumes from settings.cfg");
        }
    }

    if (renderer && !settingsPanel_.msaaSettingsApplied_) {
        if (settingsPanel_.pendingAntiAliasing > 0) {
            static const VkSampleCountFlagBits aaSamples[] = {
                VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT,
                VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT
            };
            const int idx = std::clamp(settingsPanel_.pendingAntiAliasing, 0, 3);
            renderer->setMsaaSamples(aaSamples[idx]);
        }
        settingsPanel_.msaaSettingsApplied_ = true;
    }

    if (renderer && !settingsPanel_.fxaaSettingsApplied_) {
        if (auto* post = renderer->getPostProcessPipeline()) {
            post->setFXAAEnabled(settingsPanel_.pendingFXAA);
        }
        settingsPanel_.fxaaSettingsApplied_ = true;
    }

    if (renderer && !settingsPanel_.waterRefractionApplied_) {
        renderer->setWaterRefractionEnabled(settingsPanel_.pendingWaterRefraction);
        settingsPanel_.waterRefractionApplied_ = true;
    }
}

void GameScreen::render(game::GameHandler& gameHandler) {
    // Apply before any Begin() calls so a scale change cannot alter style
    // metrics halfway through an ImGui frame.
    settingsPanel_.applyWindowUiScale();

    // Set up chat bubble callback (once) and cache game handler in ChatPanel
    chatPanel_.setupCallbacks(gameHandler);
    toastManager_.setupCallbacks(gameHandler);

    // Set up appearance-changed callback to refresh inventory preview (barber shop, etc.)
    if (!appearanceCallbackSet_) {
        gameHandler.setAppearanceChangedCallback([this]() {
            inventoryScreenCharGuid_ = 0;  // force preview re-sync on next frame
        });
        appearanceCallbackSet_ = true;
    }

    // Set up UI error frame callback (once)
    if (!uiErrorCallbackSet_) {
        gameHandler.setUIErrorCallback([this](const std::string& msg) {
            uiErrors_.push_back({msg, 0.0f});
            if (uiErrors_.size() > 5) uiErrors_.erase(uiErrors_.begin());
            // Play error sound for each new error (rate-limited by deque cap of 5)
            if (auto* ac = services_.audioCoordinator) {
                if (auto* sfx = ac->getUiSoundManager()) sfx->playError();
            }
        });
        uiErrorCallbackSet_ = true;
    }

    // Flash the action bar button whose spell just failed (0.5 s red overlay).
    if (!castFailedCallbackSet_) {
        gameHandler.setSpellCastFailedCallback([this](uint32_t spellId) {
            if (spellId == 0) return;
            float now = static_cast<float>(ImGui::GetTime());
            actionBarPanel_.actionFlashEndTimes_[spellId] = now + actionBarPanel_.kActionFlashDuration;
        });
        castFailedCallbackSet_ = true;
    }

    // Apply UI transparency setting
    float prevAlpha = ImGui::GetStyle().Alpha;
    ImGui::GetStyle().Alpha = settingsPanel_.uiOpacity_;

    // Sync minimap opacity with UI opacity
    {
        auto* renderer = services_.renderer;
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                minimap->setOpacity(settingsPanel_.uiOpacity_);
            }
        }
    }

    // Apply initial settings when renderer becomes available
    if (!settingsPanel_.minimapSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            if (auto* minimap = renderer->getMinimap()) {
                settingsPanel_.minimapRotate_ = false;
                settingsPanel_.pendingMinimapRotate = false;
                minimap->setRotateWithCamera(false);
                minimap->setSquareShape(settingsPanel_.minimapSquare_);
                settingsPanel_.minimapSettingsApplied_ = true;
            }
            if (auto* zm = renderer->getZoneManager()) {
                zm->setUseOriginalSoundtrack(settingsPanel_.pendingUseOriginalSoundtrack);
            }
            if (auto* tm = renderer->getTerrainManager()) {
                tm->setGroundClutterDensityScale(static_cast<float>(settingsPanel_.pendingGroundClutterDensity) / 100.0f);
            }
            // Restore mute state: save actual master volume first, then apply mute
            if (settingsPanel_.soundMuted_) {
                float actual = audio::AudioEngine::instance().getMasterVolume();
                settingsPanel_.preMuteVolume_ = (actual > 0.0f) ? actual
                    : static_cast<float>(settingsPanel_.pendingMasterVolume) / 100.0f;
                audio::AudioEngine::instance().setMasterVolume(0.0f);
            }
        }
    }

    // Settings are loaded before renderer services are injected. Apply the
    // saved lighting state once the renderer exists so opening the settings
    // window cannot become the first operation that changes scene lighting.
    if (!settingsPanel_.lightingSettingsApplied_) {
        if (auto* renderer = services_.renderer) {
            renderer->setShadowsEnabled(settingsPanel_.pendingShadows);
            renderer->setShadowDistance(settingsPanel_.pendingShadowDistance);
            renderer->setViewDistance(settingsPanel_.pendingViewDistance);
            if (auto* post = renderer->getPostProcessPipeline()) {
                post->setBrightness(static_cast<float>(settingsPanel_.pendingBrightness) / 50.0f);
            }
            settingsPanel_.lightingSettingsApplied_ = true;
        }
    }

    // Apply saved volume settings once when audio managers first become available
    if (!settingsPanel_.volumeSettingsApplied_) {
        auto* ac = services_.audioCoordinator;
        if (ac && ac->getUiSoundManager()) {
            settingsPanel_.applyAudioVolumes(ac);
            settingsPanel_.volumeSettingsApplied_ = true;
        }
    }

    // Apply saved MSAA setting once when renderer is available
    if (!settingsPanel_.msaaSettingsApplied_ && settingsPanel_.pendingAntiAliasing > 0) {
        auto* renderer = services_.renderer;
        if (renderer) {
            static const VkSampleCountFlagBits aaSamples[] = {
                VK_SAMPLE_COUNT_1_BIT, VK_SAMPLE_COUNT_2_BIT,
                VK_SAMPLE_COUNT_4_BIT, VK_SAMPLE_COUNT_8_BIT
            };
            renderer->setMsaaSamples(aaSamples[settingsPanel_.pendingAntiAliasing]);
            settingsPanel_.msaaSettingsApplied_ = true;
        }
    } else {
        settingsPanel_.msaaSettingsApplied_ = true;
    }

    // Apply saved FXAA setting once when renderer is available
    if (!settingsPanel_.fxaaSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            renderer->getPostProcessPipeline()->setFXAAEnabled(settingsPanel_.pendingFXAA);
            settingsPanel_.fxaaSettingsApplied_ = true;
        }
    }

    // Apply saved water refraction setting once when renderer is available
    if (!settingsPanel_.waterRefractionApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            renderer->setWaterRefractionEnabled(settingsPanel_.pendingWaterRefraction);
            settingsPanel_.waterRefractionApplied_ = true;
        }
    }

    // Apply saved normal mapping / POM settings once when WMO renderer is available
    if (!settingsPanel_.normalMapSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
            if (auto* wr = renderer->getWMORenderer()) {
                wr->setNormalMappingEnabled(settingsPanel_.pendingNormalMapping);
                wr->setNormalMapStrength(settingsPanel_.pendingNormalMapStrength);
                wr->setPOMEnabled(settingsPanel_.pendingPOM);
                wr->setPOMQuality(settingsPanel_.pendingPOMQuality);
                if (auto* cr = renderer->getCharacterRenderer()) {
                    cr->setNormalMappingEnabled(settingsPanel_.pendingNormalMapping);
                    cr->setNormalMapStrength(settingsPanel_.pendingNormalMapStrength);
                    cr->setPOMEnabled(settingsPanel_.pendingPOM);
                    cr->setPOMQuality(settingsPanel_.pendingPOMQuality);
                }
                settingsPanel_.normalMapSettingsApplied_ = true;
            }
        }
    }

    // Apply saved upscaling setting once when renderer is available
    if (!settingsPanel_.fsrSettingsApplied_) {
        auto* renderer = services_.renderer;
        if (renderer) {
#ifdef __APPLE__
            // FidelityFX and AMD frame generation are unsupported through the
            // macOS MoltenVK path. Old settings files must not silently retain
            // either feature after the controls are hidden.
            settingsPanel_.pendingUpscalingMode = 0;
            settingsPanel_.pendingFSR = false;
            settingsPanel_.pendingAMDFramegen = false;
            renderer->getPostProcessPipeline()->setAmdFsr3FramegenEnabled(false);
            renderer->setFSREnabled(false);
            renderer->setFSR2Enabled(false);
            settingsPanel_.fsrSettingsApplied_ = true;
#else
            static constexpr float fsrScales[] = { 0.77f, 0.67f, 0.59f, 1.00f };
            settingsPanel_.pendingFSRQuality = std::clamp(settingsPanel_.pendingFSRQuality, 0, 3);
            renderer->getPostProcessPipeline()->setFSRQuality(fsrScales[settingsPanel_.pendingFSRQuality]);
            renderer->getPostProcessPipeline()->setFSRSharpness(settingsPanel_.pendingFSRSharpness);
            renderer->getPostProcessPipeline()->setFSR2DebugTuning(settingsPanel_.pendingFSR2JitterSign, settingsPanel_.pendingFSR2MotionVecScaleX, settingsPanel_.pendingFSR2MotionVecScaleY);
            renderer->getPostProcessPipeline()->setAmdFsr3FramegenEnabled(settingsPanel_.pendingAMDFramegen);
            int effectiveMode = settingsPanel_.pendingUpscalingMode;

            // Defer FSR2/FSR3 activation until fully in-world to avoid
            // init issues during login/character selection screens.
            if (effectiveMode == 2 && gameHandler.getState() != game::WorldState::IN_WORLD) {
                renderer->setFSREnabled(false);
                renderer->setFSR2Enabled(false);
            } else {
                renderer->setFSREnabled(effectiveMode == 1);
                renderer->setFSR2Enabled(effectiveMode == 2);
                settingsPanel_.fsrSettingsApplied_ = true;
            }
#endif
        }
    }

    // Apply auto-loot / auto-sell settings to GameHandler every frame (cheap bool sync)
    gameHandler.setAutoLoot(settingsPanel_.pendingAutoLoot);
    gameHandler.setAutoSellGrey(settingsPanel_.pendingAutoSellGrey);
    gameHandler.setAutoRepair(settingsPanel_.pendingAutoRepair);

    // Sync chat auto-join settings to GameHandler
    gameHandler.chatAutoJoin.general = chatPanel_.chatAutoJoinGeneral;
    gameHandler.chatAutoJoin.trade = chatPanel_.chatAutoJoinTrade;
    gameHandler.chatAutoJoin.localDefense = chatPanel_.chatAutoJoinLocalDefense;
    gameHandler.chatAutoJoin.lfg = chatPanel_.chatAutoJoinLFG;
    gameHandler.chatAutoJoin.local = chatPanel_.chatAutoJoinLocal;

    // Process targeting input before UI windows
    processTargetInput(gameHandler);

    const bool originalUi = originalInterfaceActive();
    if (!originalUi) renderPlayerFrame(gameHandler);

    // Pet frame (below player frame, only when player has an active pet)
    if (gameHandler.hasPet()) {
        renderPetFrame(gameHandler);
    }

    // Auto-open pet rename modal when server signals the pet is renameable (first tame)
    if (gameHandler.consumePetRenameablePending()) {
        petRenameOpen_ = true;
        petRenameBuf_[0] = '\0';
    }

    // Totem frame (Shaman only, when any totem is active)
    if (gameHandler.getPlayerClass() == 7) {
        renderTotemFrame(gameHandler);
    }

    // Target frame (only when we have a target)
    if (gameHandler.hasTarget()) {
        renderTargetFrame(gameHandler);
    }

    // Focus target frame (only when we have a focus)
    if (gameHandler.hasFocus()) {
        renderFocusFrame(gameHandler);
    }

    // Render windows
    if (showPlayerInfo) {
        renderPlayerInfo(gameHandler);
    }

    if (showEntityWindow) {
        renderEntityList(gameHandler);
    }

    if (showChatWindow) {
        chatPanel_.getSpellIcon = [this](uint32_t id, pipeline::AssetManager* am) {
            return getSpellIcon(id, am);
        };
        if (!chatPanel_.saveSettingsFn)
            chatPanel_.saveSettingsFn = [this]() { saveSettings(); };
        chatPanel_.render(gameHandler, inventoryScreen, spellbookScreen, questLogScreen);
        // Process slash commands that affect GameScreen state
        auto cmds = chatPanel_.consumeSlashCommands();
        if (cmds.showInspect) socialPanel_.showInspectWindow_ = true;
        if (cmds.toggleThreat) combatUI_.showThreatWindow_ = !combatUI_.showThreatWindow_;
        if (cmds.showBgScore) combatUI_.showBgScoreboard_ = !combatUI_.showBgScoreboard_;
        if (cmds.showGmTicket) windowManager_.showGmTicketWindow_ = true;
        if (cmds.showWho) socialPanel_.showWhoWindow_ = true;
        if (cmds.toggleCombatLog) combatUI_.showCombatLog_ = !combatUI_.showCombatLog_;
        if (cmds.takeScreenshot) takeScreenshot(gameHandler);
    }

    // ---- New UI elements ----
    if (!originalUi) {
        actionBarPanel_.renderActionBar(gameHandler, settingsPanel_, chatPanel_,
            inventoryScreen, spellbookScreen, questLogScreen,
            [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); });
        actionBarPanel_.renderStanceBar(gameHandler, settingsPanel_, spellbookScreen,
            [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); });
        if (actionBarPanel_.renderBagBar(gameHandler, settingsPanel_, inventoryScreen))
            saveSettings();
        renderMicroMenu(gameHandler);
        actionBarPanel_.renderXpBar(gameHandler, settingsPanel_);
        actionBarPanel_.renderRepBar(gameHandler, settingsPanel_);
    }
    auto spellIconFn = [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); };
    combatUI_.renderCastBar(gameHandler, spellIconFn);
    renderMirrorTimers(gameHandler);
    combatUI_.renderCooldownTracker(gameHandler, settingsPanel_, spellIconFn);
    if (!originalUi) renderQuestObjectiveTracker(gameHandler);
    renderNameplates(gameHandler);  // player names always shown; NPC plates gated by showNameplates_
    combatUI_.renderBattlegroundScore(gameHandler);
    combatUI_.renderRaidWarningOverlay(gameHandler);
    combatUI_.renderCombatText(gameHandler);
    combatUI_.renderDPSMeter(gameHandler, settingsPanel_, lastTargetFrameBottom_);
    renderDurabilityWarning(gameHandler);
    renderUIErrors(gameHandler, ImGui::GetIO().DeltaTime);
    toastManager_.renderEarlyToasts(ImGui::GetIO().DeltaTime, gameHandler);
    if (socialPanel_.showRaidFrames_) {
        socialPanel_.renderPartyFrames(gameHandler, chatPanel_, spellIconFn);
    }
    socialPanel_.renderBossFrames(gameHandler, spellbookScreen, spellIconFn);
    dialogManager_.renderDialogs(gameHandler, inventoryScreen, chatPanel_);
    socialPanel_.renderGuildRoster(gameHandler, chatPanel_);
    socialPanel_.renderSocialFrame(gameHandler, chatPanel_);
    combatUI_.renderBuffBar(gameHandler, spellbookScreen, inventoryScreen, settingsPanel_, spellIconFn);
    windowManager_.renderLootWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderGossipWindow(gameHandler, chatPanel_);
    windowManager_.renderQuestDetailsWindow(gameHandler, chatPanel_, inventoryScreen);
    windowManager_.renderQuestRequestItemsWindow(gameHandler, chatPanel_, inventoryScreen);
    windowManager_.renderQuestOfferRewardWindow(gameHandler, chatPanel_, inventoryScreen);
    windowManager_.renderVendorWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderTrainerWindow(gameHandler,
        [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); },
        inventoryScreen);
    windowManager_.renderCraftingWindow(gameHandler,
        [this](uint32_t id, pipeline::AssetManager* am) { return getSpellIcon(id, am); },
        inventoryScreen);
    windowManager_.renderBarberShopWindow(gameHandler);
    windowManager_.renderStableWindow(gameHandler);
    // Flight selection is handled by the world map's flight-map mode (see
    // renderWorldMap); the legacy list window remains as a fallback when the
    // world map system is unavailable.
    {
        auto* mapRenderer = core::Application::getInstance().getRenderer();
        if (!mapRenderer || !mapRenderer->getWorldMap()) {
            windowManager_.renderTaxiWindow(gameHandler);
        }
    }
    windowManager_.renderMailWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderMailComposeWindow(gameHandler, inventoryScreen);
    if (windowManager_.renderBankWindow(gameHandler, inventoryScreen, chatPanel_))
        saveSettings();
    windowManager_.renderGuildBankWindow(gameHandler, inventoryScreen, chatPanel_);
    windowManager_.renderGmCommandScreen(gameHandler);
    windowManager_.renderAuctionHouseWindow(gameHandler, inventoryScreen, chatPanel_);
    socialPanel_.renderDungeonFinderWindow(gameHandler, chatPanel_);
    windowManager_.renderInstanceLockouts(gameHandler);
    socialPanel_.renderWhoWindow(gameHandler, chatPanel_);
    combatUI_.renderCombatLog(gameHandler, spellbookScreen);
    windowManager_.renderAchievementWindow(gameHandler);
    windowManager_.renderSkillsWindow(gameHandler);
    windowManager_.renderTitlesWindow(gameHandler);
    windowManager_.renderEquipSetWindow(gameHandler);
    windowManager_.renderGmTicketWindow(gameHandler);
    socialPanel_.renderInspectWindow(gameHandler, inventoryScreen);
    windowManager_.renderBookWindow(gameHandler);
    combatUI_.renderThreatWindow(gameHandler);
    combatUI_.renderBgScoreboard(gameHandler);
    if (showMinimap_) {
        renderMinimapMarkers(gameHandler);
    }
    windowManager_.renderLogoutCountdown(gameHandler);
    windowManager_.renderDeathScreen(gameHandler);
    windowManager_.renderReclaimCorpseButton(gameHandler);
    dialogManager_.renderLateDialogs(gameHandler);
    chatPanel_.renderBubbles(gameHandler);
    windowManager_.renderEscapeMenu(settingsPanel_);
    settingsPanel_.renderSettingsWindow(inventoryScreen, chatPanel_, [this]() { saveSettings(); });
    toastManager_.renderLateToasts(gameHandler);
    renderWeatherOverlay(gameHandler);

    renderWorldMap(gameHandler);

    questLogScreen.render(gameHandler, inventoryScreen);

    spellbookScreen.render(gameHandler, services_.assetManager);

    // Insert spell link into chat if player shift-clicked a spellbook entry
    {
        std::string pendingSpellLink = spellbookScreen.getAndClearPendingChatLink();
        if (!pendingSpellLink.empty()) {
            chatPanel_.insertChatLink(pendingSpellLink);
        }
    }

    // Talents (N key toggle handled inside)
    talentScreen.render(gameHandler);

    // Set up inventory screen asset manager + player appearance (re-init on character switch)
    {
        uint64_t activeGuid = gameHandler.getActiveCharacterGuid();
        if (activeGuid != 0 && activeGuid != inventoryScreenCharGuid_) {
            auto* am = services_.assetManager;
            if (am) {
                inventoryScreen.setAssetManager(am);
                const auto* ch = gameHandler.getActiveCharacter();
                if (ch) {
                    uint8_t skin = ch->appearanceBytes & 0xFF;
                    uint8_t face = (ch->appearanceBytes >> 8) & 0xFF;
                    uint8_t hairStyle = (ch->appearanceBytes >> 16) & 0xFF;
                    uint8_t hairColor = (ch->appearanceBytes >> 24) & 0xFF;
                    inventoryScreen.setPlayerAppearance(
                        ch->race, ch->gender, skin, face,
                        hairStyle, hairColor, ch->facialFeatures);
                    inventoryScreenCharGuid_ = activeGuid;
                }
            }
        }
    }

    // Set vendor mode before rendering inventory
    inventoryScreen.setVendorMode(gameHandler.isVendorWindowOpen(), &gameHandler);

    // Auto-open bags once when vendor window first opens
    if (gameHandler.isVendorWindowOpen()) {
        if (!windowManager_.vendorBagsOpened_) {
            windowManager_.vendorBagsOpened_ = true;
            if (inventoryScreen.isSeparateBags()) {
                inventoryScreen.openAllBags();
            } else if (!inventoryScreen.isOpen()) {
                inventoryScreen.setOpen(true);
            }
        }
    } else {
        windowManager_.vendorBagsOpened_ = false;
    }

    // Auto-open bags once when the guild bank first opens, so items can be
    // right-clicked to deposit into the vault.
    if (gameHandler.isGuildBankOpen()) {
        if (!windowManager_.guildBankBagsOpened_) {
            windowManager_.guildBankBagsOpened_ = true;
            if (inventoryScreen.isSeparateBags()) {
                inventoryScreen.openAllBags();
            } else if (!inventoryScreen.isOpen()) {
                inventoryScreen.setOpen(true);
            }
        }
    } else {
        windowManager_.guildBankBagsOpened_ = false;
    }

    inventoryScreen.setGameHandler(&gameHandler);
    inventoryScreen.render(gameHandler.getInventory(), gameHandler.getMoneyCopper());

    // Character screen (C key toggle handled inside render())
    inventoryScreen.renderCharacterScreen(gameHandler);

    // Item-target cursor (sharpening stone / oil awaiting the item it applies to)
    inventoryScreen.renderItemTargetCursor();

    // Insert item link into chat if player shift-clicked any inventory/equipment slot
    {
        std::string pendingLink = inventoryScreen.getAndClearPendingChatLink();
        if (!pendingLink.empty()) {
            chatPanel_.insertChatLink(pendingLink);
        }
    }

    if (inventoryScreen.consumeEquipmentDirty() || gameHandler.consumeOnlineEquipmentDirty()) {
        updateCharacterGeosets(gameHandler.getInventory());
        updateCharacterTextures(gameHandler.getInventory());
        if (appearanceComposer_) appearanceComposer_->loadEquippedWeapons();
        inventoryScreen.markPreviewDirty();
        // Update renderer weapon type for animation selection
        auto* r = services_.renderer;
        if (r) {
            const auto& mh = gameHandler.getInventory().getEquipSlot(game::EquipSlot::MAIN_HAND);
            const auto& oh = gameHandler.getInventory().getEquipSlot(game::EquipSlot::OFF_HAND);
            if (mh.empty()) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedWeaponType(0, false);
            } else {
                // Polearms and staves use ATTACK_2H_LOOSE instead of ATTACK_2H
                bool is2HLoose = (mh.item.subclassName == "Polearm" || mh.item.subclassName == "Staff");
                bool isFist = (mh.item.subclassName == "Fist Weapon");
                bool isDagger = (mh.item.subclassName == "Dagger");
                bool hasOffHand = !oh.empty() &&
                    (oh.item.inventoryType == game::InvType::ONE_HAND ||
                     oh.item.subclassName == "Fist Weapon");
                bool hasShield = !oh.empty() && oh.item.inventoryType == game::InvType::SHIELD;
                if (auto* ac = r->getAnimationController()) ac->setEquippedWeaponType(mh.item.inventoryType, is2HLoose, isFist, isDagger, hasOffHand, hasShield);
            }
            // Detect ranged weapon type from RANGED slot
            const auto& rangedSlot = gameHandler.getInventory().getEquipSlot(game::EquipSlot::RANGED);
            if (rangedSlot.empty()) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::NONE);
            } else if (rangedSlot.item.inventoryType == game::InvType::RANGED_BOW) {
                // subclassName distinguishes Bow vs Crossbow
                if (rangedSlot.item.subclassName == "Crossbow") {
                    if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::CROSSBOW);
                } else {
                    if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::BOW);
                }
            } else if (rangedSlot.item.inventoryType == game::InvType::RANGED_GUN) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::GUN);
            } else if (rangedSlot.item.inventoryType == game::InvType::THROWN) {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::THROWN);
            } else {
                if (auto* ac = r->getAnimationController()) ac->setEquippedRangedType(rendering::RangedWeaponType::NONE);
            }
        }
    }

    // Update renderer face-target position and selection circle
    auto* renderer = services_.renderer;
    if (renderer) {
        if (auto* ac = renderer->getAnimationController()) ac->setInCombat(gameHandler.isInCombat() &&
                              !gameHandler.isPlayerDead() &&
                              !gameHandler.isPlayerGhost());
        if (auto* cr = renderer->getCharacterRenderer()) {
            uint32_t charInstId = renderer->getCharacterInstanceId();
            if (charInstId != 0) {
                const bool isGhost = gameHandler.isPlayerGhost();
                if (!ghostOpacityStateKnown_ ||
                    ghostOpacityLastState_ != isGhost ||
                    ghostOpacityLastInstanceId_ != charInstId) {
                    cr->setInstanceOpacity(charInstId, isGhost ? 0.5f : 1.0f);
                    ghostOpacityStateKnown_ = true;
                    ghostOpacityLastState_ = isGhost;
                    ghostOpacityLastInstanceId_ = charInstId;
                }
            }
        }
        static glm::vec3 targetGLPos;
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target) {
                // Prefer the renderer's actual instance position so the selection
                // circle tracks the rendered model (not a parallel entity-space
                // interpolator that can drift from the visual position).
                glm::vec3 instPos;
                if (core::Application::getInstance().getRenderPositionForGuid(target->getGuid(), instPos)) {
                    targetGLPos = instPos;
                    // Override Z with foot position to sit the circle on the ground.
                    float footZ = 0.0f;
                    if (core::Application::getInstance().getRenderFootZForGuid(target->getGuid(), footZ)) {
                        targetGLPos.z = footZ;
                    }
                } else {
                    // Fallback: entity game-logic position (no CharacterRenderer instance yet)
                    targetGLPos = core::coords::canonicalToRender(
                        glm::vec3(target->getX(), target->getY(), target->getZ()));
                    float footZ = 0.0f;
                    if (core::Application::getInstance().getRenderFootZForGuid(target->getGuid(), footZ)) {
                        targetGLPos.z = footZ;
                    }
                }
                if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(&targetGLPos);

                // Selection circle color: WoW-canonical level-based colors
                bool showSelectionCircle = false;
                glm::vec3 circleColor(1.0f, 1.0f, 0.3f); // default yellow
                float circleRadius = 1.5f;
                {
                    glm::vec3 boundsCenter;
                    float boundsRadius = 0.0f;
                    if (core::Application::getInstance().getRenderBoundsForGuid(target->getGuid(), boundsCenter, boundsRadius)) {
                        float r = boundsRadius * 1.1f;
                        circleRadius = std::min(std::max(r, 0.8f), 8.0f);
                    }
                }
                if (target->getType() == game::ObjectType::UNIT) {
                    showSelectionCircle = true;
                    auto unit = std::static_pointer_cast<game::Unit>(target);
                    if (unit->getHealth() == 0 && unit->getMaxHealth() > 0) {
                        circleColor = glm::vec3(0.5f, 0.5f, 0.5f); // gray (dead)
                    } else if (unit->isHostile() || gameHandler.isAggressiveTowardPlayer(target->getGuid())) {
                        uint32_t playerLv = gameHandler.getPlayerLevel();
                        uint32_t mobLv = unit->getLevel();
                        int32_t diff = static_cast<int32_t>(mobLv) - static_cast<int32_t>(playerLv);
                        if (game::GameHandler::killXp(playerLv, mobLv) == 0) {
                            circleColor = glm::vec3(0.6f, 0.6f, 0.6f); // grey
                        } else if (diff >= 10) {
                            circleColor = glm::vec3(1.0f, 0.1f, 0.1f); // red
                        } else if (diff >= 5) {
                            circleColor = glm::vec3(1.0f, 0.5f, 0.1f); // orange
                        } else if (diff >= -2) {
                            circleColor = glm::vec3(1.0f, 1.0f, 0.1f); // yellow
                        } else {
                            circleColor = glm::vec3(0.3f, 1.0f, 0.3f); // green
                        }
                    } else {
                        circleColor = glm::vec3(0.3f, 1.0f, 0.3f); // green (friendly)
                    }
                } else if (target->getType() == game::ObjectType::PLAYER) {
                    showSelectionCircle = true;
                    circleColor = glm::vec3(0.3f, 1.0f, 0.3f); // green (player)
                } else if (target->getType() == game::ObjectType::GAMEOBJECT) {
                    showSelectionCircle = true;
                    circleColor = glm::vec3(0.2f, 0.8f, 1.0f); // cyan (game object)
                }
                if (showSelectionCircle) {
                    renderer->setSelectionCircle(targetGLPos, circleRadius, circleColor);
                } else {
                    renderer->clearSelectionCircle();
                }
            } else {
                if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(nullptr);
                renderer->clearSelectionCircle();
            }
        } else {
            if (auto* ac = renderer->getAnimationController()) ac->setTargetPosition(nullptr);
            renderer->clearSelectionCircle();
        }
    }

    // Screen edge damage flash — red vignette that fires on HP decrease
    {
        const bool deadOrGhost = gameHandler.isPlayerDead() || gameHandler.isPlayerGhost();
        auto playerEntity = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
        uint32_t currentHp = 0;
        if (playerEntity && (playerEntity->getType() == game::ObjectType::PLAYER ||
                             playerEntity->getType() == game::ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<game::Unit>(playerEntity);
            if (unit->getMaxHealth() > 0)
                currentHp = unit->getHealth();
        }

        // Detect HP drop (ignore transitions from 0 — entity just spawned or uninitialized)
        if (!deadOrGhost && settingsPanel_.damageFlashEnabled_ &&
            lastPlayerHp_ > 0 && currentHp < lastPlayerHp_ && currentHp > 0) {
            damageFlashAlpha_ = 1.0f;
        }
        lastPlayerHp_ = currentHp;

        // Spirit release can leave a low/non-zero health value on the local
        // entity. Never carry a pre-death damage flash into ghost form.
        if (deadOrGhost) {
            damageFlashAlpha_ = 0.0f;
        }

        // Fade out over ~0.5 seconds
        if (damageFlashAlpha_ > 0.0f) {
            damageFlashAlpha_ -= ImGui::GetIO().DeltaTime * 2.0f;
            if (damageFlashAlpha_ < 0.0f) damageFlashAlpha_ = 0.0f;
            drawScreenEdgeVignette(200, 0, 0,
                                   static_cast<int>(damageFlashAlpha_ * 100.0f), 0.12f);
        }
    }

    // Persistent low-health vignette — pulsing red edges when HP < 20%
    {
        auto playerEntity = gameHandler.getEntityManager().getEntity(gameHandler.getPlayerGuid());
        const bool deadOrGhost = gameHandler.isPlayerDead() || gameHandler.isPlayerGhost();
        float hpPct = 1.0f;
        if (!deadOrGhost && playerEntity &&
            (playerEntity->getType() == game::ObjectType::PLAYER ||
             playerEntity->getType() == game::ObjectType::UNIT)) {
            auto unit = std::static_pointer_cast<game::Unit>(playerEntity);
            if (unit->getMaxHealth() > 0)
                hpPct = static_cast<float>(unit->getHealth()) / static_cast<float>(unit->getMaxHealth());
        }

        // Only show when alive and below 20% HP; intensity increases as HP drops
        if (settingsPanel_.lowHealthVignetteEnabled_ && !deadOrGhost &&
            hpPct < 0.20f && hpPct > 0.0f) {
            // Base intensity from HP deficit (0 at 20%, 1 at 0%); pulse at ~1.5 Hz
            float danger = (0.20f - hpPct) / 0.20f;
            float pulse  = 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 9.4f);
            int   alpha  = static_cast<int>(danger * pulse * 90.0f);  // max ~90 alpha, subtle
            drawScreenEdgeVignette(200, 0, 0, alpha, 0.15f);
        }
    }

    // Level-up golden burst overlay
    if (toastManager_.levelUpFlashAlpha > 0.0f) {
        toastManager_.levelUpFlashAlpha -= ImGui::GetIO().DeltaTime * 1.0f;  // fade over ~1 second
        if (toastManager_.levelUpFlashAlpha < 0.0f) toastManager_.levelUpFlashAlpha = 0.0f;

        const int alpha = static_cast<int>(toastManager_.levelUpFlashAlpha * 160.0f);
        drawScreenEdgeVignette(255, 210, 50, alpha, 0.18f);

        // "Level X!" text in the center during the first half of the animation
        if (toastManager_.levelUpFlashAlpha > 0.5f && toastManager_.levelUpDisplayLevel > 0) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const float W = ImGui::GetIO().DisplaySize.x;
            const float H = ImGui::GetIO().DisplaySize.y;
            char lvlText[32];
            snprintf(lvlText, sizeof(lvlText), "Level %u!", toastManager_.levelUpDisplayLevel);
            ImVec2 ts = ImGui::CalcTextSize(lvlText);
            float tx = (W - ts.x) * 0.5f;
            float ty = H * 0.35f;
            // Large shadow + bright gold text
            fg->AddText(nullptr, 28.0f, ImVec2(tx + 2, ty + 2), IM_COL32(0, 0, 0, alpha), lvlText);
            fg->AddText(nullptr, 28.0f, ImVec2(tx, ty), IM_COL32(255, 230, 80, alpha), lvlText);
        }
    }

    // Restore previous alpha
    ImGui::GetStyle().Alpha = prevAlpha;
}

void GameScreen::renderMicroMenu(game::GameHandler& gameHandler) {
    if (!settingsPanel_.pendingShowMicroMenu) return;

    ImGuiIO& io = ImGui::GetIO();
#ifdef __ANDROID__
    const float buttonSize = 44.0f;
#else
    const float buttonSize = 28.0f;
#endif
    constexpr float margin = 10.0f;
    const float actionBarH = 48.0f * settingsPanel_.pendingActionBarScale + 24.0f;
    const float y = std::max(8.0f, io.DisplaySize.y - actionBarH - buttonSize - 16.0f);

    ImGui::SetNextWindowPos(ImVec2(margin, y), ImGuiCond_Always);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(8, 8, 12, 145));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(120, 130, 165, 90));

    if (ImGui::Begin("##MicroMenu", nullptr, flags)) {
        auto button = [&](const char* label, const char* tooltip, bool active) {
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(64, 82, 132, 210));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(82, 102, 160, 230));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(98, 120, 180, 245));
            }
            const bool clicked = ImGui::Button(label, ImVec2(buttonSize, buttonSize));
            if (active) ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
            return clicked;
        };

        if (button("C##MicroCharacter", "Character", inventoryScreen.isCharacterOpen())) {
            const bool wasOpen = inventoryScreen.isCharacterOpen();
            inventoryScreen.toggleCharacter();
            if (!wasOpen && gameHandler.isConnected()) gameHandler.requestPlayedTime();
        }
        ImGui::SameLine();
        if (button("B##MicroBags", "Backpack", inventoryScreen.isBackpackOpen())) {
            inventoryScreen.toggleBackpack();
        }
        ImGui::SameLine();
        if (button("P##MicroSpellbook", "Spellbook", spellbookScreen.isOpen())) {
            spellbookScreen.toggle();
        }
        ImGui::SameLine();
        if (button("N##MicroTalents", "Talents", talentScreen.isOpen())) {
            talentScreen.toggle();
        }
        ImGui::SameLine();
        if (button("L##MicroQuests", "Quest Log", questLogScreen.isOpen())) {
            questLogScreen.toggle();
        }
        ImGui::SameLine();
        if (button("K##MicroSkills", "Skills", windowManager_.showSkillsWindow_)) {
            windowManager_.showSkillsWindow_ = !windowManager_.showSkillsWindow_;
        }
        ImGui::SameLine();
        if (button("O##MicroSocial", "Social", socialPanel_.showSocialFrame_)) {
            socialPanel_.showSocialFrame_ = !socialPanel_.showSocialFrame_;
        }
        ImGui::SameLine();
        if (button("G##MicroGroup", "Party/Raid Frames", socialPanel_.showRaidFrames_)) {
            socialPanel_.showRaidFrames_ = !socialPanel_.showRaidFrames_;
        }
        ImGui::SameLine();
        if (button("M##MicroMap", "World Map", showWorldMap_)) {
            showWorldMap_ = !showWorldMap_;
        }
        ImGui::SameLine();
        if (button("GM##MicroGM", "GM Commands", windowManager_.showGmCommandScreen_)) {
            windowManager_.showGmCommandScreen_ = !windowManager_.showGmCommandScreen_;
        }
        ImGui::SameLine();
        if (button("*##MicroSettings", "Settings", settingsPanel_.showSettingsWindow)) {
            settingsPanel_.showSettingsWindow = !settingsPanel_.showSettingsWindow;
        }
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void GameScreen::renderPlayerInfo(game::GameHandler& gameHandler) {
    ImGui::SetNextWindowSize(ImVec2(350, 250), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::Begin("Player Info", &showPlayerInfo);

    const auto& movement = gameHandler.getMovementInfo();

    ImGui::Text("Position & Movement");
    ImGui::Separator();
    ImGui::Spacing();

    // Position
    ImGui::Text("Position:");
    ImGui::Indent();
    ImGui::Text("X: %.2f", movement.x);
    ImGui::Text("Y: %.2f", movement.y);
    ImGui::Text("Z: %.2f", movement.z);
    ImGui::Text("Orientation: %.2f rad (%.1f deg)", movement.orientation, movement.orientation * 180.0f / 3.14159f);
    ImGui::Unindent();

    ImGui::Spacing();

    // Movement flags
    ImGui::Text("Movement Flags: 0x%08X", movement.flags);
    ImGui::Text("Time: %u ms", movement.time);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Connection state
    ImGui::Text("Connection State:");
    ImGui::Indent();
    auto state = gameHandler.getState();
    switch (state) {
        case game::WorldState::IN_WORLD:
            ImGui::TextColored(kColorBrightGreen, "In World");
            break;
        case game::WorldState::AUTHENTICATED:
            ImGui::TextColored(kColorYellow, "Authenticated");
            break;
        case game::WorldState::ENTERING_WORLD:
            ImGui::TextColored(kColorYellow, "Entering World...");
            break;
        default:
            ImGui::TextColored(kColorRed, "State: %d", static_cast<int>(state));
            break;
    }
    ImGui::Unindent();

    ImGui::End();
}

void GameScreen::renderEntityList(game::GameHandler& gameHandler) {
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 290), ImGuiCond_FirstUseEver);
    ImGui::Begin("Entities", &showEntityWindow);

    const auto& entityManager = gameHandler.getEntityManager();
    const auto& entities = entityManager.getEntities();

    ImGui::Text("Entities in View: %zu", entities.size());
    ImGui::Separator();
    ImGui::Spacing();

    if (entities.empty()) {
        ImGui::TextDisabled("No entities in view");
    } else {
        // Entity table
        if (ImGui::BeginTable("EntitiesTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("GUID", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            const auto& playerMovement = gameHandler.getMovementInfo();
            float playerX = playerMovement.x;
            float playerY = playerMovement.y;
            float playerZ = playerMovement.z;

            for (const auto& [guid, entity] : entities) {
                ImGui::TableNextRow();

                // GUID
                ImGui::TableSetColumnIndex(0);
                char guidStr[24];
                snprintf(guidStr, sizeof(guidStr), "0x%016llX", (unsigned long long)guid);
                ImGui::Text("%s", guidStr);

                // Type
                ImGui::TableSetColumnIndex(1);
                switch (entity->getType()) {
                    case game::ObjectType::PLAYER:
                        ImGui::TextColored(kColorBrightGreen, "Player");
                        break;
                    case game::ObjectType::UNIT:
                        ImGui::TextColored(kColorYellow, "Unit");
                        break;
                    case game::ObjectType::GAMEOBJECT:
                        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "GameObject");
                        break;
                    default:
                        ImGui::Text("Object");
                        break;
                }

                // Name (for players and units)
                ImGui::TableSetColumnIndex(2);
                if (entity->getType() == game::ObjectType::PLAYER) {
                    auto player = std::static_pointer_cast<game::Player>(entity);
                    ImGui::Text("%s", player->getName().c_str());
                } else if (entity->getType() == game::ObjectType::UNIT) {
                    auto unit = std::static_pointer_cast<game::Unit>(entity);
                    if (!unit->getName().empty()) {
                        ImGui::Text("%s", unit->getName().c_str());
                    } else {
                        ImGui::TextDisabled("--");
                    }
                } else {
                    ImGui::TextDisabled("--");
                }

                // Position
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%.1f, %.1f, %.1f", entity->getX(), entity->getY(), entity->getZ());

                // Distance from player
                ImGui::TableSetColumnIndex(4);
                float dx = entity->getX() - playerX;
                float dy = entity->getY() - playerY;
                float dz = entity->getZ() - playerZ;
                float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                ImGui::Text("%.1f", distance);
            }

            ImGui::EndTable();
        }
    }

    ImGui::End();
}

void GameScreen::processTargetInput(game::GameHandler& gameHandler) {
    auto& io = ImGui::GetIO();
    auto& input = core::Input::getInstance();

    // If the user is typing (or about to focus chat this frame), do not allow
    // A-Z or 1-0 shortcuts to fire.
    if (!io.WantTextInput && !chatPanel_.isChatInputActive() && input.isKeyJustPressed(SDL_SCANCODE_SLASH)) {
        chatPanel_.activateSlashInput();
    }
    if (!io.WantTextInput && !chatPanel_.isChatInputActive() &&
        KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_CHAT, false)) {
        chatPanel_.activateInput();
    }

    const bool textFocus = chatPanel_.isChatInputActive() || io.WantTextInput;

    // Game hotkeys — gate on textFocus (chat/text-input active) rather than
    // WantCaptureKeyboard so that toggle keys like M, C, I still work when an
    // ImGui window (character panel, map, etc.) happens to have focus.
    {
        if (!textFocus && input.isKeyJustPressed(SDL_SCANCODE_TAB)) {
            const auto& movement = gameHandler.getMovementInfo();
            gameHandler.tabTarget(movement.x, movement.y, movement.z);
        }

        // Escape (TOGGLE_SETTINGS) must not fire while chat input is active —
        // otherwise pressing Escape to close chat also closes any open window or
        // opens the escape menu, since ImGui deactivates InputText on Escape but
        // the same press still propagates here. KeybindingManager only blocks
        // A-Z/0-9 during text input, not Escape.
        if (!textFocus &&
            KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_SETTINGS, true)) {
            if (settingsPanel_.showSettingsWindow) {
                settingsPanel_.showSettingsWindow = false;
            } else if (windowManager_.showEscapeMenu) {
                windowManager_.showEscapeMenu = false;
                settingsPanel_.showEscapeSettingsNotice = false;
            } else if (gameHandler.isCasting()) {
                gameHandler.cancelCast();
            } else if (gameHandler.isLootWindowOpen()) {
                gameHandler.closeLoot();
            } else if (gameHandler.isGossipWindowOpen()) {
                gameHandler.closeGossip();
            } else if (gameHandler.isVendorWindowOpen()) {
                gameHandler.closeVendor();
            } else if (gameHandler.isBarberShopOpen()) {
                gameHandler.closeBarberShop();
            } else if (gameHandler.isBankOpen()) {
                gameHandler.closeBank();
            } else if (gameHandler.isGuildBankOpen()) {
                gameHandler.closeGuildBank();
            } else if (gameHandler.isTrainerWindowOpen()) {
                gameHandler.closeTrainer();
            } else if (gameHandler.isMailboxOpen()) {
                gameHandler.closeMailbox();
            } else if (gameHandler.isAuctionHouseOpen()) {
                gameHandler.closeAuctionHouse();
            } else if (gameHandler.isQuestDetailsOpen()) {
                gameHandler.declineQuest();
            } else if (gameHandler.isQuestOfferRewardOpen()) {
                gameHandler.closeQuestOfferReward();
            } else if (gameHandler.isQuestRequestItemsOpen()) {
                gameHandler.closeQuestRequestItems();
            } else if (gameHandler.isTradeOpen()) {
                gameHandler.cancelTrade();
            } else {
                windowManager_.showEscapeMenu = true;
            }
        }

        if (!textFocus) {
            // Toggle character screen (C) and inventory/bags (I)
            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_CHARACTER_SCREEN)) {
                const bool wasOpen = inventoryScreen.isCharacterOpen();
                inventoryScreen.toggleCharacter();
                if (!wasOpen && gameHandler.isConnected()) {
                    gameHandler.requestPlayedTime();
                }
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_INVENTORY)) {
                inventoryScreen.toggle();
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_NAMEPLATES)) {
                if (ImGui::GetIO().KeyShift)
                    settingsPanel_.showFriendlyNameplates_ = !settingsPanel_.showFriendlyNameplates_;
                else
                    showNameplates_ = !showNameplates_;
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_WORLD_MAP)) {
                showWorldMap_ = !showWorldMap_;
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_MINIMAP)) {
                showMinimap_ = !showMinimap_;
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_RAID_FRAMES)) {
                socialPanel_.showRaidFrames_ = !socialPanel_.showRaidFrames_;
            }

            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_ACHIEVEMENTS)) {
                windowManager_.showAchievementWindow_ = !windowManager_.showAchievementWindow_;
            }
            if (KeybindingManager::getInstance().isActionPressed(KeybindingManager::Action::TOGGLE_SKILLS)) {
                windowManager_.showSkillsWindow_ = !windowManager_.showSkillsWindow_;
            }

            // Toggle Titles window with H (hero/title screen — no conflicting keybinding)
            if (input.isKeyJustPressed(SDL_SCANCODE_H)) {
                windowManager_.showTitlesWindow_ = !windowManager_.showTitlesWindow_;
            }

            // Screenshot (PrintScreen key)
            if (input.isKeyJustPressed(SDL_SCANCODE_PRINTSCREEN)) {
                takeScreenshot(gameHandler);
            }

            // Action bar keys (1-9, 0, -, =)
            static const SDL_Scancode actionBarKeys[] = {
                SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
                SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
                SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS
            };
            const bool shiftDown = input.isKeyPressed(SDL_SCANCODE_LSHIFT) || input.isKeyPressed(SDL_SCANCODE_RSHIFT);
            const bool ctrlDown  = input.isKeyPressed(SDL_SCANCODE_LCTRL)  || input.isKeyPressed(SDL_SCANCODE_RCTRL);
            const auto& bar = gameHandler.getActionBar();

            // Ctrl+1..Ctrl+8 → switch stance/form/presence (WoW default bindings).
            // Only fires for classes that use a stance bar; same slot ordering as
            // renderStanceBar: Warrior, DK, Druid, Rogue, Priest.
            if (ctrlDown) {
                static constexpr uint32_t warriorStances[]  = { 2457, 71, 2458 };
                static constexpr uint32_t dkPresences[]     = { 48266, 48263, 48265 };
                static constexpr uint32_t druidForms[]      = { 5487, 9634, 768, 783, 1066, 24858, 33891, 33943, 40120 };
                static constexpr uint32_t rogueForms[]      = { 1784 };
                static constexpr uint32_t priestForms[]     = { 15473 };
                const uint32_t* stArr = nullptr; int stCnt = 0;
                switch (gameHandler.getPlayerClass()) {
                    case 1:  stArr = warriorStances; stCnt = 3; break;
                    case 6:  stArr = dkPresences;    stCnt = 3; break;
                    case 11: stArr = druidForms;     stCnt = 9; break;
                    case 4:  stArr = rogueForms;     stCnt = 1; break;
                    case 5:  stArr = priestForms;    stCnt = 1; break;
                }
                if (stArr) {
                    const auto& known = gameHandler.getKnownSpells();
                    // Build available list (same order as UI)
                    std::vector<uint32_t> avail;
                    avail.reserve(stCnt);
                    for (int i = 0; i < stCnt; ++i)
                        if (known.count(stArr[i])) avail.push_back(stArr[i]);
                    // Ctrl+1 = first stance, Ctrl+2 = second, …
                    for (int i = 0; i < static_cast<int>(avail.size()) && i < 8; ++i) {
                        if (input.isKeyJustPressed(actionBarKeys[i]))
                            gameHandler.castSpell(avail[i]);
                    }
                }
            }

            for (int i = 0; i < game::GameHandler::SLOTS_PER_BAR; ++i) {
                if (!ctrlDown && input.isKeyJustPressed(actionBarKeys[i])) {
                    int slotIdx = shiftDown
                        ? ActionBarPanel::actionSlotForPage(ActionBarPanel::kBottomLeftActionPage, i)
                        : ActionBarPanel::actionSlotForPage(actionBarPanel_.getMainActionBarPage(), i);
                    if (bar[slotIdx].type == game::ActionBarSlot::SPELL && bar[slotIdx].isReady()) {
                        uint64_t target = gameHandler.hasTarget() ? gameHandler.getTargetGuid() : 0;
                        gameHandler.castSpell(bar[slotIdx].id, target);
                    } else if (bar[slotIdx].type == game::ActionBarSlot::ITEM && bar[slotIdx].id != 0) {
                        gameHandler.useItemById(bar[slotIdx].id);
                    } else if (bar[slotIdx].type == game::ActionBarSlot::MACRO) {
                        chatPanel_.executeMacroText(gameHandler, gameHandler.getMacroText(bar[slotIdx].id));
                    }
                }
            }
        }

    }

    // Cursor affordance: show hand cursor over interactable entities.
    if (!io.WantCaptureMouse) {
        auto* renderer = services_.renderer;
        auto* camera = renderer ? renderer->getCamera() : nullptr;
        auto* window = services_.window;
        if (camera && window) {
            glm::vec2 mousePos = input.getMousePosition();
            float screenW = static_cast<float>(window->getWidth());
            float screenH = static_cast<float>(window->getHeight());
            rendering::Ray ray = camera->screenToWorldRay(mousePos.x, mousePos.y, screenW, screenH);
            // The same picker the click uses, so the cursor affordance cannot
            // disagree with what clicking would actually select — including the
            // tighter sphere critters get, which this copy did not have.
            const ui::ScenePick hoverPick =
                ui::pickScene(gameHandler, ray, ui::ScenePickParams{});
            if (hoverPick.closestGuid != 0) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }
    }

    // Left-click targeting: only on mouse-up if the mouse didn't drag (camera rotate)
    // Record press position on mouse-down
    if (!io.WantCaptureMouse && input.isMouseButtonJustPressed(SDL_BUTTON_LEFT) && !input.isMouseButtonPressed(SDL_BUTTON_RIGHT)) {
        leftClickPressPos_ = input.getMousePosition();
        leftClickWasPress_ = true;
    }

    // On mouse-up, check if it was a click (not a drag)
    if (leftClickWasPress_ && input.isMouseButtonJustReleased(SDL_BUTTON_LEFT)) {
        leftClickWasPress_ = false;
        glm::vec2 releasePos = input.getMousePosition();
        glm::vec2 dragDelta = releasePos - leftClickPressPos_;
        float dragDistSq = glm::dot(dragDelta, dragDelta);
        const float CLICK_THRESHOLD = clickDragThreshold();

        if (dragDistSq < CLICK_THRESHOLD * CLICK_THRESHOLD) {
            auto* renderer = services_.renderer;
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = services_.window;

            if (camera && window) {
                float screenW = static_cast<float>(window->getWidth());
                float screenH = static_cast<float>(window->getHeight());

                rendering::Ray ray = camera->screenToWorldRay(leftClickPressPos_.x, leftClickPressPos_.y, screenW, screenH);

                // Use the same authoritative position and forgiving click volume
                // as right-click for a hooked bobber. Its tiny, partly submerged
                // render bounds must not let a click fall through to a character.
                const uint64_t hookedBobber = gameHandler.getHookedFishingBobberGuid();
                if (hookedBobber != 0) {
                    auto bobber = gameHandler.getEntityManager().getEntity(hookedBobber);
                    if (bobber && bobber->getType() == game::ObjectType::GAMEOBJECT) {
                        glm::vec3 bobberCenter = core::coords::canonicalToRender(
                            glm::vec3(bobber->getX(), bobber->getY(), bobber->getZ()));
                        bobberCenter.z += 0.35f;
                        float hitT = 0.0f;
                        constexpr float kFishingBobberClickRadius = 4.0f;
                        if (raySphereIntersect(ray, bobberCenter, kFishingBobberClickRadius, hitT)) {
                            LOG_WARNING("Fishing bobber direct left click: guid=0x", std::hex,
                                        hookedBobber, std::dec, " hitDistance=", hitT);
                            gameHandler.interactWithGameObject(hookedBobber);
                            return;
                        }
                    }
                }

                const ui::ScenePick pick = ui::pickScene(
                    gameHandler, ray, ui::ScenePickParams{});
                uint64_t closestGuid = pick.resolve();

                if (closestGuid != 0) {
                    if (closestGuid == gameHandler.getHookedFishingBobberGuid()) {
                        gameHandler.interactWithGameObject(closestGuid);
                    } else {
                        gameHandler.setTarget(closestGuid);
                    }
                } else {
                    // Clicked empty space — deselect current target
                    gameHandler.clearTarget();
                }
            }
        }
    }

    // Right-click: select NPC (if needed) then interact / loot / auto-attack.
    // Record the press position; the action only fires on release for a tap (below),
    // never for a right-drag camera rotate — otherwise turning the view toward a nearby
    // mob would auto-attack it without the player intending to engage.
    if (!io.WantCaptureMouse && input.isMouseButtonJustPressed(SDL_BUTTON_RIGHT) && !input.isMouseButtonPressed(SDL_BUTTON_LEFT)) {
        rightClickPressPos_ = input.getMousePosition();
        rightClickWasPress_ = true;
    }

    // On right mouse-up, act only if it was a click (not a drag / camera rotate).
    if (rightClickWasPress_ && input.isMouseButtonJustReleased(SDL_BUTTON_RIGHT)) {
        rightClickWasPress_ = false;
        glm::vec2 rDragDelta = input.getMousePosition() - rightClickPressPos_;
        const float RCLICK_THRESHOLD = clickDragThreshold();
        if (glm::dot(rDragDelta, rDragDelta) >= RCLICK_THRESHOLD * RCLICK_THRESHOLD) {
            // Treated as a camera rotate — do not interact/attack.
            return;
        }
        // Fishing bobbers are tiny and partly submerged, so their model bounds can
        // miss a cursor ray that visibly lands on the float. Test the authoritative
        // water position first with a forgiving sphere and reel directly, before
        // normal unit targeting can fall through to the player/self target.
        const uint64_t hookedBobber = gameHandler.getHookedFishingBobberGuid();
        if (hookedBobber != 0) {
            auto bobber = gameHandler.getEntityManager().getEntity(hookedBobber);
            auto* renderer = services_.renderer;
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = services_.window;
            if (bobber && bobber->getType() == game::ObjectType::GAMEOBJECT && camera && window) {
                const glm::vec2 mousePos = input.getMousePosition();
                const rendering::Ray ray = camera->screenToWorldRay(
                    mousePos.x, mousePos.y,
                    static_cast<float>(window->getWidth()),
                    static_cast<float>(window->getHeight()));
                glm::vec3 bobberCenter = core::coords::canonicalToRender(
                    glm::vec3(bobber->getX(), bobber->getY(), bobber->getZ()));
                bobberCenter.z += 0.35f;
                float hitT = 0.0f;
                constexpr float kFishingBobberClickRadius = 4.0f;
                if (raySphereIntersect(ray, bobberCenter, kFishingBobberClickRadius, hitT)) {
                    LOG_WARNING("Fishing bobber direct click: guid=0x", std::hex,
                                hookedBobber, std::dec, " hitDistance=", hitT);
                    gameHandler.interactWithGameObject(hookedBobber);
                    return;
                }
            }
        }

        // If a gameobject is already targeted, prioritize interacting with that target
        // instead of re-picking under cursor (which can hit nearby decorative GOs).
        // Exclude chair-type GOs (type 7): otherwise any right-click (including the
        // start of a camera rotate) auto-sits the player whenever a chair is targeted.
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target && target->getType() == game::ObjectType::GAMEOBJECT) {
                auto go = std::static_pointer_cast<game::GameObject>(target);
                auto* goInfo = gameHandler.getCachedGameObjectInfo(go->getEntry());
                if (!goInfo || goInfo->type != 7) {
                    LOG_DEBUG("[GO-DIAG] Right-click: re-interacting with targeted GO 0x",
                                std::hex, target->getGuid(), std::dec);
                    gameHandler.setTarget(target->getGuid());
                    gameHandler.interactWithGameObject(target->getGuid());
                    return;
                }
            }
        }

        // If no target or right-clicking in world, try to pick one under cursor
        {
            auto* renderer = services_.renderer;
            auto* camera = renderer ? renderer->getCamera() : nullptr;
            auto* window = services_.window;
            if (camera && window) {
                // If a quest objective gameobject is under the cursor, prefer it over
                // hostile units so quest pickups (e.g. "Bundle of Wood") are reliable.
                std::unordered_set<uint32_t> questObjectiveGoEntries;
                {
                    const auto& ql = gameHandler.getQuestLog();
                    questObjectiveGoEntries.reserve(32);
                    for (const auto& q : ql) {
                        if (q.complete) continue;
                        for (const auto& obj : q.killObjectives) {
                            if (obj.npcOrGoId >= 0 || obj.required == 0) continue;
                            uint32_t entry = static_cast<uint32_t>(-obj.npcOrGoId);
                            uint32_t cur = 0;
                            auto it = q.killCounts.find(entry);
                            if (it != q.killCounts.end()) cur = it->second.first;
                            if (cur < obj.required) questObjectiveGoEntries.insert(entry);
                        }
                    }
                }

                glm::vec2 mousePos = input.getMousePosition();
                float screenW = static_cast<float>(window->getWidth());
                float screenH = static_cast<float>(window->getHeight());
                rendering::Ray ray = camera->screenToWorldRay(mousePos.x, mousePos.y, screenW, screenH);
                // The right-click picker wants a slightly taller, tighter fallback
                // sphere than the target picker, and must ignore chairs: their wide
                // sphere is easy to catch while right-drag-rotating the camera, which
                // sits the player down.
                ui::ScenePickParams params;
                params.unitHitRadius = 1.5f;
                params.unitHeightOffset = 1.5f;
                params.skipChairs = true;

                float closestQuestGoT = 1e30f;
                uint64_t closestQuestGoGuid = 0;
                float hookedBobberT = 1e30f;
                uint64_t hookedBobberGuid = 0;
                const ui::ScenePick pick = ui::pickScene(
                    gameHandler, ray, params,
                    [&](uint64_t guid, const std::shared_ptr<game::Entity>& entity,
                        float hitT, float /*centerT*/) {
                        if (entity->getType() != game::ObjectType::GAMEOBJECT) return;
                        if (guid == gameHandler.getHookedFishingBobberGuid() &&
                            hitT < hookedBobberT) {
                            hookedBobberT = hitT;
                            hookedBobberGuid = guid;
                        }
                        if (questObjectiveGoEntries.empty()) return;
                        auto go = std::static_pointer_cast<game::GameObject>(entity);
                        if (questObjectiveGoEntries.count(go->getEntry()) &&
                            hitT < closestQuestGoT) {
                            closestQuestGoT = hitT;
                            closestQuestGoGuid = guid;
                        }
                    });

                uint64_t closestGuid = 0;
                game::ObjectType closestType = game::ObjectType::OBJECT;

                // A hooked fishing bobber is time-sensitive and intentionally small:
                // if its click sphere was hit, reel it rather than selecting an
                // overlapping unit or decorative object.
                if (hookedBobberGuid != 0) {
                    closestGuid = hookedBobberGuid;
                    closestType = game::ObjectType::GAMEOBJECT;
                } else if (closestQuestGoGuid != 0) {
                    closestGuid = closestQuestGoGuid;
                    closestType = game::ObjectType::GAMEOBJECT;
                } else {
                    closestGuid = pick.resolve();
                    closestType = (closestGuid != 0 && closestGuid == pick.objectGuid)
                                      ? game::ObjectType::GAMEOBJECT
                                      : game::ObjectType::UNIT;
                }

                if (closestGuid != 0) {
                    if (closestType == game::ObjectType::GAMEOBJECT) {
                        LOG_DEBUG("[GO-DIAG] Right-click: raypick hit GO 0x",
                                    std::hex, closestGuid, std::dec);
                        gameHandler.setTarget(closestGuid);
                        gameHandler.interactWithGameObject(closestGuid);
                        return;
                    }
                    gameHandler.setTarget(closestGuid);
                }
            }
        }
        if (gameHandler.hasTarget()) {
            auto target = gameHandler.getTarget();
            if (target) {
                if (target->getType() == game::ObjectType::UNIT) {
                    // Check if unit is dead (health == 0) → loot, otherwise interact/attack
                    auto unit = std::static_pointer_cast<game::Unit>(target);
                    if (unit->getHealth() == 0 && unit->getMaxHealth() > 0) {
                        gameHandler.lootTarget(target->getGuid());
                    } else {
                        // Interact with service NPCs; otherwise treat non-interactable living units
                        // as attackable fallback (covers bad faction-template classification).
                        auto isSpiritNpc = [&]() -> bool {
                            constexpr uint32_t NPC_FLAG_SPIRIT_GUIDE = 0x00004000;
                            constexpr uint32_t NPC_FLAG_SPIRIT_HEALER = 0x00008000;
                            if (unit->getNpcFlags() & (NPC_FLAG_SPIRIT_GUIDE | NPC_FLAG_SPIRIT_HEALER)) {
                                return true;
                            }
                            std::string name = unit->getName();
                            std::transform(name.begin(), name.end(), name.begin(),
                                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                            return (name.find("spirit healer") != std::string::npos) ||
                                   (name.find("spirit guide") != std::string::npos);
                        };
                        bool allowSpiritInteract = (gameHandler.isPlayerDead() || gameHandler.isPlayerGhost()) && isSpiritNpc();
                        bool canInteractNpc = unit->isInteractable() || allowSpiritInteract;
                        bool shouldAttackByFallback = !canInteractNpc;
                        if (!unit->isHostile() && canInteractNpc) {
                            gameHandler.interactWithNpc(target->getGuid());
                        } else if (unit->isHostile() || shouldAttackByFallback) {
                            gameHandler.startAutoAttack(target->getGuid());
                        }
                    }
                } else if (target->getType() == game::ObjectType::GAMEOBJECT) {
                    // Skip chairs: auto-sit must only happen when the chair is
                    // under the cursor, never as a side effect of right-click.
                    auto go = std::static_pointer_cast<game::GameObject>(target);
                    auto* goInfo = gameHandler.getCachedGameObjectInfo(go->getEntry());
                    if (!goInfo || goInfo->type != 7) {
                        gameHandler.interactWithGameObject(target->getGuid());
                    }
                } else if (target->getType() == game::ObjectType::PLAYER) {
                    // Right-click another player could start attack in PvP context
                }
            }
        }
    }
}


}} // namespace wowee::ui
