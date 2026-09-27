#pragma once

#include "game/game_handler.hpp"
#include "game/inventory.hpp"
// WorldMap is now owned by Renderer, accessed via getWorldMap()
#include "rendering/character_preview.hpp"
#include "ui/inventory_screen.hpp"
#include "ui/quest_log_screen.hpp"
#include "ui/spellbook_screen.hpp"
#include "ui/talent_screen.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/chat_panel.hpp"
#include "ui/toast_manager.hpp"
#include "ui/dialog_manager.hpp"
#include "ui/settings_panel.hpp"
#include "ui/combat_ui.hpp"
#include "ui/social_panel.hpp"
#include "ui/action_bar_panel.hpp"
#include "ui/window_manager.hpp"
#include "ui/ui_services.hpp"
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace core { class AppearanceComposer; }
namespace pipeline { class AssetManager; }
namespace rendering { class Renderer; }
namespace ui {

/**
 * In-game screen UI
 *
 * Displays player info, entity list, chat, and game controls
 */
class GameScreen {
public:
    GameScreen();

    /**
     * Render the UI
     * @param gameHandler Reference to game handler
     */
    void render(game::GameHandler& gameHandler);

    /// Equipped model for CharacterModelFrame. Width and height are the texture
    /// size, used to letterbox it into the frame.
    VkDescriptorSet paperDollTexture(game::GameHandler& gameHandler, int& width, int& height);
    void setPaperDollFacing(float radians);
    /// The 1.12 world-map micro button. The FrameXML map is a full-screen
    /// panel, and opening one hides the rest of the interface.
    void toggleWorldMap();

    /**
     * Check if chat input is active
     */
    bool isChatInputActive() const { return chatPanel_.isChatInputActive(); }

    void saveSettings();
    void loadSettings();

    // Dependency injection for extracted classes (Phase A singleton breaking)
    void setAppearanceComposer(core::AppearanceComposer* ac) { appearanceComposer_ = ac; }

    // UIServices injection (Phase B singleton breaking)
    void setServices(const UIServices& services);

private:
    void applyCameraControlSettings();
    void applyStartupGraphicsAndAudio();

    // Injected UI services (Section 3.5 Phase B - replaces getInstance() calls)
    UIServices services_;
    // Legacy pointer for Phase A compatibility (will be removed when all callsites migrate)
    core::AppearanceComposer* appearanceComposer_ = nullptr;
    // Chat panel (extracted from GameScreen — owns all chat state and rendering)
    ChatPanel chatPanel_;

    // Toast manager (extracted from GameScreen — owns all toast/notification state and rendering)
    ToastManager toastManager_;

    // Dialog manager (extracted from GameScreen — owns all popup/dialog rendering)
    DialogManager dialogManager_;

    // Settings panel (extracted from GameScreen — owns all settings UI and config state)
    SettingsPanel settingsPanel_;

    // Combat UI (extracted from GameScreen — owns all combat overlay rendering)
    CombatUI combatUI_;

    // Social panel (extracted from GameScreen — owns all social/group UI rendering)
    SocialPanel socialPanel_;

    // Action bar panel (extracted from GameScreen — owns action/stance/bag/xp/rep bars)
    ActionBarPanel actionBarPanel_;

    // Window manager (extracted from GameScreen — owns NPC windows, popups, overlays)
    WindowManager windowManager_;

    // UI state
    bool showEntityWindow = false;
    bool showChatWindow = true;
    bool showMinimap_ = true;  // M key toggles minimap
    bool showNameplates_ = true;  // V key toggles enemy/NPC nameplates
    uint64_t nameplateCtxGuid_ = 0; // GUID of nameplate right-clicked (0 = none)
    ImVec2 nameplateCtxPos_{};      // Screen position of nameplate right-click
    uint32_t lastPlayerHp_ = 0;   // Previous frame HP for damage flash detection
    float damageFlashAlpha_ = 0.0f; // Screen edge flash intensity (fades to 0)


    // UIErrorsFrame: WoW-style center-bottom error messages (spell fails, out of range, etc.)
    struct UIErrorEntry { std::string text; float age = 0.0f; };
    std::vector<UIErrorEntry> uiErrors_;
    bool uiErrorCallbackSet_ = false;
    static constexpr float kUIErrorLifetime = 2.5f;
    bool castFailedCallbackSet_ = false;

    bool showPlayerInfo = false;
    bool showWorldMap_ = false;  // W key toggles world map
    ImVec2 questTrackerPos_ = ImVec2(-1.0f, -1.0f);  // <0 = use default
    ImVec2 questTrackerSize_ = ImVec2(220.0f, 200.0f); // saved size
    float questTrackerRightOffset_ = -1.0f;            // pixels from right edge; <0 = use default
    bool questTrackerPosInit_ = false;
    int questTrackerFilter_ = 3;                        // 0=All, 1=Active, 2=Done, 3=Zone (default)
    bool questTrackerCollapsed_ = false;                // collapsed to floating bubble



    /**
     * Render player info window
     */
    void renderPlayerInfo(game::GameHandler& gameHandler);

    /**
     * Render entity list window
     */
    void renderEntityList(game::GameHandler& gameHandler);

    /**
     * Render player unit frame (top-left)
     */
    void renderPlayerFrame(game::GameHandler& gameHandler);

    /**
     * Render target frame
     */
    void renderTargetFrame(game::GameHandler& gameHandler);
    // Last measured width of the auto-sizing target frame, used to keep it centered
    // (position is set before the window lays out, so it lags by one frame).
    float lastTargetFrameWidth_ = 250.0f;
    // Screen Y of the target frame's bottom edge, or -1 when nothing is
    // targeted. The DPS meter parks itself just below this.
    float lastTargetFrameBottom_ = -1.0f;
    // Halves of the saved DPS meter position, applied once both have been read
    // (settings arrive as separate key/value lines).
    float dpsMeterSavedX_ = -1.0f;
    float dpsMeterSavedY_ = -1.0f;
    void renderFocusFrame(game::GameHandler& gameHandler);

    /**
     * Render pet frame (below player frame when player has an active pet)
     */
    void renderPetFrame(game::GameHandler& gameHandler);
    void renderTotemFrame(game::GameHandler& gameHandler);

    /**
     * Process targeting input (Tab, Escape, click)
     */
    void processTargetInput(game::GameHandler& gameHandler);

    /**
     * Rebuild character geosets from current equipment state
     */
    void updateCharacterGeosets(game::Inventory& inventory);

    /**
     * Re-composite character skin texture from current equipment
     */
    void updateCharacterTextures(game::Inventory& inventory);


    void renderMirrorTimers(game::GameHandler& gameHandler);
    void renderUIErrors(game::GameHandler& gameHandler, float deltaTime);
    void renderQuestMarkers(game::GameHandler& gameHandler);
    void renderMinimapMarkers(game::GameHandler& gameHandler);
    void refreshQuestObjectiveCache(game::GameHandler& gameHandler);
    void renderMicroMenu(game::GameHandler& gameHandler);
    void renderQuestObjectiveTracker(game::GameHandler& gameHandler);
    void renderNameplates(game::GameHandler& gameHandler);
    void renderDurabilityWarning(game::GameHandler& gameHandler);
    void takeScreenshot(game::GameHandler& gameHandler);

    /**
     * Inventory screen
     */
    void renderWorldMap(game::GameHandler& gameHandler);

    InventoryScreen inventoryScreen;
    uint64_t inventoryScreenCharGuid_ = 0;  // GUID of character inventory screen was initialized for
    QuestLogScreen questLogScreen;
    SpellbookScreen spellbookScreen;
    TalentScreen talentScreen;
    // WorldMap is now owned by Renderer (accessed via renderer->getWorldMap())

    // Spell icon cache: spellId -> GL texture ID
    std::unordered_map<uint32_t, VkDescriptorSet> spellIconCache_;
    // SpellIconID -> icon path (from SpellIcon.dbc)
    std::unordered_map<uint32_t, std::string> spellIconPaths_;
    // SpellID -> SpellIconID (from the active expansion's Spell.dbc layout)
    std::unordered_map<uint32_t, uint32_t> spellIconIds_;
    bool spellIconDbLoaded_ = false;
    VkDescriptorSet getSpellIcon(uint32_t spellId, pipeline::AssetManager* am);

    // Minimap quest-objective cache, rebuilt only when tracked quest progress changes.
    uint64_t minimapQuestCacheSignature_ = 0;
    std::unordered_set<uint32_t> minimapQuestCreatureEntries_;
    std::unordered_set<uint32_t> minimapQuestGameObjectEntries_;

    // Death Knight rune bar: client-predicted fill (0.0=depleted, 1.0=ready) for smooth animation
    float runeClientFill_[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

    // Pet rename modal (triggered from pet frame context menu)
    bool petRenameOpen_ = false;
    char petRenameBuf_[16] = {};

    // Left-click targeting: distinguish click from camera drag
    glm::vec2 leftClickPressPos_ = glm::vec2(0.0f);
    bool leftClickWasPress_ = false;
    // Right-click interact/attack fires on release only if the press was a tap, not a
    // camera-rotation drag — so turning the view near a mob doesn't auto-attack it.
    glm::vec2 rightClickPressPos_ = glm::vec2(0.0f);
    bool rightClickWasPress_ = false;


    bool appearanceCallbackSet_ = false;
    bool ghostOpacityStateKnown_ = false;
    bool ghostOpacityLastState_ = false;
    uint32_t ghostOpacityLastInstanceId_ = 0;


    void renderWeatherOverlay(game::GameHandler& gameHandler);

public:
    void openDungeonFinder() { socialPanel_.showDungeonFinder_ = true; }
    ToastManager& toastManager() { return toastManager_; }
};

} // namespace ui
} // namespace wowee
