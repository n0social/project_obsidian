#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <deque>
#include <algorithm>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class Camera;
class VkContext;
class VkTexture;
class VkRenderTarget;

class Minimap {
public:
    Minimap();
    ~Minimap();

    bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, int size = 200);
    void shutdown();
    void recreatePipelines();

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }
    void setMapName(const std::string& name);

    /// Off-screen composite pass — call BEFORE the main render pass begins.
    void compositePass(VkCommandBuffer cmd, const glm::vec3& centerWorldPos);

    /// Display quad — call INSIDE the main render pass.
    void render(VkCommandBuffer cmd, const Camera& playerCamera,
                const glm::vec3& centerWorldPos, int screenWidth, int screenHeight,
                float playerOrientation = 0.0f, bool hasPlayerOrientation = false);

    void setEnabled(bool enabled) { this->enabled = enabled; }
    bool isEnabled() const { return enabled; }
    void toggle() { enabled = !enabled; }

    void setViewRadius(float radius) { viewRadius = radius; }
    void setRotateWithCamera(bool rotate) { rotateWithCamera = rotate; }
    bool isRotateWithCamera() const { return rotateWithCamera; }

    void setSquareShape(bool square) { squareShape = square; }
    bool isSquareShape() const { return squareShape; }
    float getViewRadius() const { return viewRadius; }

    void zoomIn() { viewRadius = std::max(100.0f, viewRadius - 50.0f); }
    void zoomOut() { viewRadius = std::min(800.0f, viewRadius + 50.0f); }

    void setOpacity(float opacity) { opacity_ = opacity; }

    /// Pixels, origin top-left. The terrain disc is drawn here instead of the
    /// fixed top-right corner. Cleared, the corner placement returns.
    void setDisplayRect(float x, float y, float w, float h) {
        displayRectSet_ = w > 1.0f && h > 1.0f;
        displayX_ = x;
        displayY_ = y;
        displayW_ = w;
        displayH_ = h;
    }
    void clearDisplayRect() { displayRectSet_ = false; }
    bool hasDisplayRect() const { return displayRectSet_; }
    void getDisplayRect(float& x, float& y, float& w, float& h) const {
        x = displayX_;
        y = displayY_;
        w = displayW_;
        h = displayH_;
    }

    /// Vanilla zoom is 0 (far) through 4 (close). Step 2 matches the default radius.
    void setZoomLevel(int zoom);
    int getZoomLevel() const { return zoomLevel_; }

    float getArrowRotation() const { return arrowRotation_; }
    VkDescriptorSet getArrowDS() const { return arrowDS_; }

    // Public accessors for WorldMap
    VkTexture* getOrLoadTileTexture(int tileX, int tileY);
    void ensureTRSParsed() { if (!trsParsed) parseTRS(); }
    const std::string& getMapName() const { return mapName; }

private:
    void parseTRS();
    void updateTileDescriptors(uint32_t frameIdx, int centerTileX, int centerTileY);

    VkContext* vkCtx = nullptr;
    pipeline::AssetManager* assetManager = nullptr;
    std::string mapName = "Azeroth";

    // TRS lookup: "Azeroth\map32_49" → "e7f0dea73ee6baca78231aaf4b7e772a"
    std::unordered_map<std::string, std::string> trsLookup;
    bool trsParsed = false;

    // Tile texture cache: hash → VkTexture
    // Evicted (FIFO) when the count of successfully-loaded tiles exceeds MAX_TILE_CACHE.
    static constexpr size_t MAX_TILE_CACHE = 128;
    std::unordered_map<std::string, std::unique_ptr<VkTexture>> tileTextureCache;
    std::deque<std::string> tileInsertionOrder;  // hashes of successfully loaded tiles, oldest first
    std::unique_ptr<VkTexture> noDataTexture;

    // Composite render target (3x3 tiles = 768x768)
    std::unique_ptr<VkRenderTarget> compositeTarget;
    static constexpr int TILE_PX = 256;
    static constexpr int COMPOSITE_PX = TILE_PX * 3;  // 768

    // Shared quad vertex buffer (6 verts, pos2 + uv2 = 16 bytes/vert)
    ::VkBuffer quadVB = VK_NULL_HANDLE;
    VmaAllocation quadVBAlloc = VK_NULL_HANDLE;

    // Descriptor resources (shared layout: 1 combined image sampler at binding 0)
    VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    static constexpr uint32_t MAX_DESC_SETS = 24;

    // Tile composite pipeline (renders into VkRenderTarget)
    VkPipeline tilePipeline = VK_NULL_HANDLE;
    VkPipelineLayout tilePipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet tileDescSets[2][9] = {};  // [frameInFlight][tileSlot]

    // Display pipeline (renders into main render pass)
    VkPipeline displayPipeline = VK_NULL_HANDLE;
    VkPipelineLayout displayPipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSet displayDescSet = VK_NULL_HANDLE;

    int mapSize = 200;
    float viewRadius = 400.0f;
    int zoomLevel_ = 2;
    bool displayRectSet_ = false;
    float displayX_ = 0.0f, displayY_ = 0.0f, displayW_ = 0.0f, displayH_ = 0.0f;
    bool enabled = true;
    bool rotateWithCamera = false;
    bool squareShape = false;
    float opacity_ = 1.0f;

    // Throttling
    float updateIntervalSec = 0.25f;
    float updateDistance = 6.0f;
    std::chrono::steady_clock::time_point lastUpdateTime{};
    glm::vec3 lastUpdatePos{0.0f};
    bool hasCachedFrame = false;

    // Tile tracking
    int lastCenterTileX = -1;
    int lastCenterTileY = -1;

    // Player arrow texture (MinimapArrow.blp)
    std::unique_ptr<VkTexture> arrowTexture_;
    VkDescriptorSet arrowDS_ = VK_NULL_HANDLE;
    bool arrowLoadAttempted_ = false;
    float arrowRotation_ = 0.0f;
};

} // namespace rendering
} // namespace wowee
