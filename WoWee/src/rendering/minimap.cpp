#include "rendering/minimap.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_render_target.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <sstream>
#include <cmath>

namespace wowee {
namespace rendering {

// Push constant for tile composite vertex shader
struct MinimapTilePush {
    glm::vec2 gridOffset;  // 8 bytes
};

// Push constant for display vertex + fragment shaders
struct MinimapDisplayPush {
    glm::vec4 rect;         // x, y, w, h in 0..1 screen space
    glm::vec2 playerUV;
    float rotation;
    float arrowRotation;
    float zoomRadius;
    int32_t squareShape;
    float opacity;
};  // 44 bytes

Minimap::Minimap() = default;

Minimap::~Minimap() {
    shutdown();
}

bool Minimap::initialize(VkContext* ctx, VkDescriptorSetLayout /*perFrameLayout*/, int size) {
    vkCtx = ctx;
    mapSize = size;
    VkDevice device = vkCtx->getDevice();

    // --- Composite render target (768x768) ---
    compositeTarget = std::make_unique<VkRenderTarget>();
    if (!compositeTarget->create(*vkCtx, COMPOSITE_PX, COMPOSITE_PX)) {
        LOG_ERROR("Minimap: failed to create composite render target");
        return false;
    }

    // --- No-data fallback texture (dark blue-gray, 1x1) ---
    noDataTexture = std::make_unique<VkTexture>();
    uint8_t darkPixel[4] = { 12, 20, 30, 255 };
    noDataTexture->upload(*vkCtx, darkPixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    noDataTexture->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

    // --- Shared quad vertex buffer (unit quad: pos2 + uv2) ---
    float quadVerts[] = {
        // pos (x,y), uv (u,v)
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 1.0f,  0.0f, 1.0f,
    };
    auto quadBuf = uploadBuffer(*vkCtx, quadVerts, sizeof(quadVerts),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    quadVB = quadBuf.buffer;
    quadVBAlloc = quadBuf.allocation;

    // --- Descriptor set layout: 1 combined image sampler at binding 0 (fragment) ---
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerSetLayout = createDescriptorSetLayout(device, { samplerBinding });

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_DESC_SETS;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_DESC_SETS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);

    // --- Allocate all descriptor sets ---
    // 18 tile sets (2 frames × 9 tiles) + 1 display set = 19 total
    std::vector<VkDescriptorSetLayout> layouts(19, samplerSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = 19;
    allocInfo.pSetLayouts = layouts.data();

    VkDescriptorSet allSets[19];
    vkAllocateDescriptorSets(device, &allocInfo, allSets);

    for (int f = 0; f < 2; f++)
        for (int t = 0; t < 9; t++)
            tileDescSets[f][t] = allSets[f * 9 + t];
    displayDescSet = allSets[18];

    // --- Write display descriptor set → composite render target ---
    VkDescriptorImageInfo compositeImgInfo = compositeTarget->descriptorInfo();
    VkWriteDescriptorSet displayWrite{};
    displayWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    displayWrite.dstSet = displayDescSet;
    displayWrite.dstBinding = 0;
    displayWrite.descriptorCount = 1;
    displayWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    displayWrite.pImageInfo = &compositeImgInfo;
    vkUpdateDescriptorSets(device, 1, &displayWrite, 0, nullptr);

    // --- Tile pipeline layout: samplerSetLayout + 8-byte push constant (vertex) ---
    VkPushConstantRange tilePush{};
    tilePush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    tilePush.offset = 0;
    tilePush.size = sizeof(MinimapTilePush);
    tilePipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { tilePush });

    // --- Display pipeline layout: samplerSetLayout + 40-byte push constant (vert+frag) ---
    VkPushConstantRange displayPush{};
    displayPush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    displayPush.offset = 0;
    displayPush.size = sizeof(MinimapDisplayPush);
    displayPipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { displayPush });

    // --- Vertex input: pos2 (loc 0) + uv2 (loc 1), stride 16 ---
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };                    // aPos
    attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float) };    // aUV

    // --- Load tile shaders ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_tile.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_tile.frag.spv")) {
            LOG_ERROR("Minimap: failed to load tile shaders");
            return false;
        }

        tilePipeline = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({ binding }, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendDisabled())
            .setLayout(tilePipelineLayout)
            .setRenderPass(compositeTarget->getRenderPass())
            .setDynamicStates({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR })
            .build(device, vkCtx->getPipelineCache());

        vs.destroy();
        fs.destroy();
    }

    // --- Load display shaders ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_display.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_display.frag.spv")) {
            LOG_ERROR("Minimap: failed to load display shaders");
            return false;
        }

        displayPipeline = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({ binding }, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
            .setMultisample(vkCtx->getMsaaSamples())
            .setLayout(displayPipelineLayout)
            .setRenderPass(vkCtx->getImGuiRenderPass())
            .setDynamicStates({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR })
            .build(device, vkCtx->getPipelineCache());

        vs.destroy();
        fs.destroy();
    }

    if (!tilePipeline || !displayPipeline) {
        LOG_ERROR("Minimap: failed to create pipelines");
        return false;
    }

    LOG_INFO("Minimap initialized (", mapSize, "x", mapSize, " screen, ",
             COMPOSITE_PX, "x", COMPOSITE_PX, " composite)");
    return true;
}

void Minimap::shutdown() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();

    vkDeviceWaitIdle(device);

    if (tilePipeline) { vkDestroyPipeline(device, tilePipeline, nullptr); tilePipeline = VK_NULL_HANDLE; }
    if (displayPipeline) { vkDestroyPipeline(device, displayPipeline, nullptr); displayPipeline = VK_NULL_HANDLE; }
    if (tilePipelineLayout) { vkDestroyPipelineLayout(device, tilePipelineLayout, nullptr); tilePipelineLayout = VK_NULL_HANDLE; }
    if (displayPipelineLayout) { vkDestroyPipelineLayout(device, displayPipelineLayout, nullptr); displayPipelineLayout = VK_NULL_HANDLE; }
    if (descPool) { vkDestroyDescriptorPool(device, descPool, nullptr); descPool = VK_NULL_HANDLE; }
    if (samplerSetLayout) { vkDestroyDescriptorSetLayout(device, samplerSetLayout, nullptr); samplerSetLayout = VK_NULL_HANDLE; }

    if (quadVB) { vmaDestroyBuffer(alloc, quadVB, quadVBAlloc); quadVB = VK_NULL_HANDLE; }

    for (auto& [hash, tex] : tileTextureCache) {
        if (tex) tex->destroy(device, alloc);
    }
    tileTextureCache.clear();
    tileInsertionOrder.clear();

    if (noDataTexture) { noDataTexture->destroy(device, alloc); noDataTexture.reset(); }
    if (compositeTarget) { compositeTarget->destroy(device, alloc); compositeTarget.reset(); }

    if (arrowDS_) { ImGui_ImplVulkan_RemoveTexture(arrowDS_); arrowDS_ = VK_NULL_HANDLE; }
    if (arrowTexture_) { arrowTexture_->destroy(device, alloc); arrowTexture_.reset(); }

    vkCtx = nullptr;
}

void Minimap::recreatePipelines() {
    if (!vkCtx || !displayPipelineLayout) return;
    VkDevice device = vkCtx->getDevice();

    if (displayPipeline) { vkDestroyPipeline(device, displayPipeline, nullptr); displayPipeline = VK_NULL_HANDLE; }

    VkShaderModule vs, fs;
    if (!vs.loadFromFile(device, "assets/shaders/minimap_display.vert.spv") ||
        !fs.loadFromFile(device, "assets/shaders/minimap_display.frag.spv")) {
        LOG_ERROR("Minimap: failed to reload display shaders for pipeline recreation");
        return;
    }

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float) };

    displayPipeline = PipelineBuilder()
        .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({ binding }, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(vkCtx->getMsaaSamples())
        .setLayout(displayPipelineLayout)
        .setRenderPass(vkCtx->getImGuiRenderPass())
        .setDynamicStates({ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR })
        .build(device, vkCtx->getPipelineCache());

    vs.destroy();
    fs.destroy();

    LOG_INFO("Minimap: display pipeline recreated with MSAA ", static_cast<int>(vkCtx->getMsaaSamples()), "x");
}

void Minimap::setMapName(const std::string& name) {
    if (mapName != name) {
        mapName = name;
        hasCachedFrame = false;
        lastCenterTileX = -1;
        lastCenterTileY = -1;
    }
}

// --------------------------------------------------------
// TRS parsing
// --------------------------------------------------------

void Minimap::parseTRS() {
    if (trsParsed || !assetManager) return;
    trsParsed = true;

    auto data = assetManager->readFile("Textures\\Minimap\\md5translate.trs");
    if (data.empty()) {
        LOG_WARNING("Failed to load md5translate.trs");
        return;
    }

    std::string content(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream stream(content);
    std::string line;
    int count = 0;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.substr(0, 4) == "dir:") continue;

        auto tabPos = line.find('\t');
        if (tabPos == std::string::npos) continue;

        std::string key = line.substr(0, tabPos);
        std::string hashFile = line.substr(tabPos + 1);

        if (key.size() > 4 && key.substr(key.size() - 4) == ".blp")
            key = key.substr(0, key.size() - 4);
        if (hashFile.size() > 4 && hashFile.substr(hashFile.size() - 4) == ".blp")
            hashFile = hashFile.substr(0, hashFile.size() - 4);

        trsLookup[key] = hashFile;
        count++;
    }

    LOG_INFO("Parsed md5translate.trs: ", count, " entries");
}

// --------------------------------------------------------
// Tile texture loading
// --------------------------------------------------------

VkTexture* Minimap::getOrLoadTileTexture(int tileX, int tileY) {
    if (!trsParsed) parseTRS();

    std::string key = mapName + "\\map" + std::to_string(tileX) + "_" + std::to_string(tileY);

    auto trsIt = trsLookup.find(key);
    if (trsIt == trsLookup.end())
        return noDataTexture.get();

    const std::string& hash = trsIt->second;

    auto cacheIt = tileTextureCache.find(hash);
    if (cacheIt != tileTextureCache.end())
        return cacheIt->second.get();

    // Load from MPQ
    std::string blpPath = "Textures\\Minimap\\" + hash + ".blp";
    auto blpImage = assetManager->loadTexture(blpPath);
    if (!blpImage.isValid()) {
        tileTextureCache[hash] = nullptr;  // Mark as failed
        return noDataTexture.get();
    }

    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx, blpImage.data.data(), blpImage.width, blpImage.height,
                VK_FORMAT_R8G8B8A8_UNORM, false);
    tex->createSampler(vkCtx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

    VkTexture* ptr = tex.get();
    tileTextureCache[hash] = std::move(tex);
    tileInsertionOrder.push_back(hash);

    // Evict oldest tiles when cache grows too large to bound GPU memory usage.
    while (tileInsertionOrder.size() > MAX_TILE_CACHE) {
        const std::string& oldest = tileInsertionOrder.front();
        tileTextureCache.erase(oldest);
        tileInsertionOrder.pop_front();
    }

    return ptr;
}

// --------------------------------------------------------
// Update tile descriptor sets for composite pass
// --------------------------------------------------------

void Minimap::updateTileDescriptors(uint32_t frameIdx, int centerTileX, int centerTileY) {
    constexpr int kTileCount = 9; // 3x3 grid
    VkDevice device = vkCtx->getDevice();
    std::array<VkDescriptorImageInfo, kTileCount> imgInfos{};
    std::array<VkWriteDescriptorSet, kTileCount> writes{};
    int slot = 0;

    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int tx = centerTileX + dr;
            int ty = centerTileY + dc;

            VkTexture* tileTex = getOrLoadTileTexture(tx, ty);
            if (!tileTex || !tileTex->isValid())
                tileTex = noDataTexture.get();

            imgInfos[slot] = tileTex->descriptorInfo();

            writes[slot] = {};
            writes[slot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[slot].dstSet = tileDescSets[frameIdx][slot];
            writes[slot].dstBinding = 0;
            writes[slot].descriptorCount = 1;
            writes[slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[slot].pImageInfo = &imgInfos[slot];
            slot++;
        }
    }

    vkUpdateDescriptorSets(device, kTileCount, writes.data(), 0, nullptr);
}

// --------------------------------------------------------
// Off-screen composite pass (call BEFORE main render pass)
// --------------------------------------------------------

void Minimap::compositePass(VkCommandBuffer cmd, const glm::vec3& centerWorldPos) {
    if (!enabled || !assetManager || !compositeTarget || !compositeTarget->isValid()) return;

    if (!trsParsed) parseTRS();

    // Check if composite needs refresh
    const auto now = std::chrono::steady_clock::now();
    bool needsRefresh = !hasCachedFrame;
    if (!needsRefresh) {
        float mdx = centerWorldPos.x - lastUpdatePos.x;
        float mdy = centerWorldPos.y - lastUpdatePos.y;
        float movedSq = mdx * mdx + mdy * mdy;
        float elapsed = std::chrono::duration<float>(now - lastUpdateTime).count();
        needsRefresh = (movedSq >= updateDistance * updateDistance) || (elapsed >= updateIntervalSec);
    }

    // Also refresh if player crossed a tile boundary
    auto [curTileX, curTileY] = core::coords::worldToTile(centerWorldPos.x, centerWorldPos.y);
    if (curTileX != lastCenterTileX || curTileY != lastCenterTileY)
        needsRefresh = true;

    if (!needsRefresh) return;

    uint32_t frameIdx = vkCtx->getCurrentFrame();

    // Update tile descriptor sets
    updateTileDescriptors(frameIdx, curTileX, curTileY);

    // Begin off-screen render pass
    VkClearColorValue clearColor = {{ 0.05f, 0.08f, 0.12f, 1.0f }};
    compositeTarget->beginPass(cmd, clearColor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tilePipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);

    // Draw 3x3 tile grid
    int slot = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    tilePipelineLayout, 0, 1,
                                    &tileDescSets[frameIdx][slot], 0, nullptr);

            MinimapTilePush push{};
            push.gridOffset = glm::vec2(static_cast<float>(dc + 1),
                                        static_cast<float>(dr + 1));
            vkCmdPushConstants(cmd, tilePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(push), &push);

            vkCmdDraw(cmd, 6, 1, 0, 0);
            slot++;
        }
    }

    compositeTarget->endPass(cmd);

    // Update tracking
    lastCenterTileX = curTileX;
    lastCenterTileY = curTileY;
    lastUpdateTime = now;
    lastUpdatePos = centerWorldPos;
    hasCachedFrame = true;
}

// --------------------------------------------------------
// Display quad (call INSIDE main render pass)
// --------------------------------------------------------

void Minimap::render(VkCommandBuffer cmd, const Camera& playerCamera,
                     const glm::vec3& centerWorldPos,
                     int screenWidth, int screenHeight,
                     float playerOrientation, bool hasPlayerOrientation) {
    if (!enabled || !hasCachedFrame || !displayPipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, displayPipeline);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            displayPipelineLayout, 0, 1,
                            &displayDescSet, 0, nullptr);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);

    // The 1.12 Minimap frame supplies a pixel rect (origin top-left). Otherwise
    // the disc stays in the top-right corner.
    float pixelW, pixelH, x, y;
    if (displayRectSet_ && screenWidth > 0 && screenHeight > 0) {
        pixelW = displayW_ / static_cast<float>(screenWidth);
        pixelH = displayH_ / static_cast<float>(screenHeight);
        x = displayX_ / static_cast<float>(screenWidth);
        y = displayY_ / static_cast<float>(screenHeight);
    } else {
        float margin = 10.0f;
        pixelW = static_cast<float>(mapSize) / screenWidth;
        pixelH = static_cast<float>(mapSize) / screenHeight;
        x = 1.0f - pixelW - margin / screenWidth;
        y = margin / screenHeight;  // top edge in Vulkan (y=0 is top)
    }

    // Compute player's UV in the composite texture
    constexpr float TILE_SIZE = core::coords::TILE_SIZE;
    auto [tileX, tileY] = core::coords::worldToTile(centerWorldPos.x, centerWorldPos.y);

    float fracNS = 32.0f - static_cast<float>(tileX) - centerWorldPos.y / TILE_SIZE;
    float fracEW = 32.0f - static_cast<float>(tileY) - centerWorldPos.x / TILE_SIZE;

    float playerU = (1.0f + fracEW) / 3.0f;
    float playerV = (1.0f + fracNS) / 3.0f;

    float zoomRadius = viewRadius / (TILE_SIZE * 3.0f);

    float rotation = 0.0f;
    if (rotateWithCamera) {
        glm::vec3 fwd = playerCamera.getForward();
        rotation = std::atan2(-fwd.x, fwd.y);
    }

    float arrowRotation = 0.0f;
    if (!rotateWithCamera) {
        if (hasPlayerOrientation) {
            arrowRotation = playerOrientation;
        } else {
            glm::vec3 fwd = playerCamera.getForward();
            arrowRotation = glm::pi<float>() - std::atan2(-fwd.x, fwd.y);
        }
    } else if (hasPlayerOrientation) {
        // Show character facing relative to the rotated map
        arrowRotation = playerOrientation + rotation;
    }

    MinimapDisplayPush push{};
    push.rect = glm::vec4(x, y, pixelW, pixelH);
    push.playerUV = glm::vec2(playerU, playerV);
    push.rotation = rotation;
    arrowRotation_ = arrowRotation;
    push.arrowRotation = arrowRotation;
    push.zoomRadius = zoomRadius;
    // The original border is round, so a frame-placed disc stays circular.
    push.squareShape = (displayRectSet_ || !squareShape) ? 0 : 1;
    push.opacity = opacity_;

    vkCmdPushConstants(cmd, displayPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdDraw(cmd, 6, 1, 0, 0);
}

void Minimap::setZoomLevel(int zoom) {
    if (zoom < 0) zoom = 0;
    if (zoom > 4) zoom = 4;
    zoomLevel_ = zoom;
    // Step 2 is the radius the corner map has always used.
    static const float kRadius[] = {800.0f, 600.0f, 400.0f, 250.0f, 140.0f};
    viewRadius = kRadius[zoom];
}

} // namespace rendering
} // namespace wowee
