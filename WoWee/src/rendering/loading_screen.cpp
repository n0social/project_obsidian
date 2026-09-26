#include "rendering/loading_screen.hpp"
#include "rendering/vk_context.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_sdl2.h>
#include <SDL2/SDL.h>
#include <random>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace wowee {
namespace rendering {

LoadingScreen::LoadingScreen() {
    // Classic glue loading art (BLP via AssetManager). PNG fallback is optional.
    imagePaths.push_back("Interface\\Glues\\LoadingScreens\\LoadScreenKalimdor.blp");
    imagePaths.push_back("Interface\\Glues\\LoadingScreens\\LoadScreenEasternKingdom.blp");
    imagePaths.push_back("Interface\\Glues\\LoadingScreens\\LoadScreenDungeon.blp");
    imagePaths.push_back("Interface\\Glues\\loading.blp");
}

LoadingScreen::~LoadingScreen() {
    shutdown();
}

bool LoadingScreen::initialize() {
    LOG_INFO("Initializing loading screen (Vulkan/ImGui)");
    selectRandomImage();
    LOG_INFO("Loading screen initialized");
    return true;
}

void LoadingScreen::shutdown() {
    if (vkCtx && bgImage) {
        VkDevice device = vkCtx->getDevice();
        vkDeviceWaitIdle(device);

        if (bgDescriptorSet) {
            // ImGui manages descriptor set lifetime
            bgDescriptorSet = VK_NULL_HANDLE;
        }
        bgSampler = VK_NULL_HANDLE; // Owned by VkContext sampler cache
        if (bgImageView) {
            vkDestroyImageView(device, bgImageView, nullptr);
            bgImageView = VK_NULL_HANDLE;
        }
        if (bgImage) {
            vkDestroyImage(device, bgImage, nullptr);
            bgImage = VK_NULL_HANDLE;
        }
        if (bgMemory) {
            vkFreeMemory(device, bgMemory, nullptr);
            bgMemory = VK_NULL_HANDLE;
        }
    }
}

void LoadingScreen::selectRandomImage() {
    if (imagePaths.empty()) {
        loadImage(std::string());
        return;
    }

    unsigned seed = static_cast<unsigned>(
        std::chrono::system_clock::now().time_since_epoch().count());
    std::default_random_engine generator(seed);
    std::uniform_int_distribution<int> distribution(0, static_cast<int>(imagePaths.size()) - 1);

    // Try a few random candidates so a missing custom PNG does not blank the screen.
    for (int attempt = 0; attempt < static_cast<int>(imagePaths.size()); ++attempt) {
        currentImageIndex = distribution(generator);
        LOG_INFO("Selected loading screen: ", imagePaths[currentImageIndex]);
        if (loadImage(imagePaths[currentImageIndex])) {
            return;
        }
    }
    LOG_WARNING("Loading screen: all candidates failed — using procedural backdrop");
    loadImage(std::string());
}

static uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOG_ERROR("LoadingScreen: no suitable memory type found");
    return UINT32_MAX;
}

bool LoadingScreen::uploadRgba(const unsigned char* data, int width, int height) {
    if (!vkCtx || !data || width <= 0 || height <= 0) return false;

    imageWidth = width;
    imageHeight = height;

    VkDevice device = vkCtx->getDevice();
    VkPhysicalDevice physDevice = vkCtx->getPhysicalDevice();
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(imageWidth) * imageHeight * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = imageSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mapped;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
        memcpy(mapped, data, static_cast<size_t>(imageSize));
        vkUnmapMemory(device, stagingMemory);
    }

    {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight), 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &imgInfo, nullptr, &bgImage);

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, bgImage, &memReqs);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemoryType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &bgMemory);
        vkBindImageMemory(device, bgImage, bgMemory, 0);
    }

    vkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = bgImage;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight), 1};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, bgImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    });

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = bgImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &viewInfo, nullptr, &bgImageView);
    }

    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        bgSampler = vkCtx->getOrCreateSampler(samplerInfo);
    }

    bgDescriptorSet = ImGui_ImplVulkan_AddTexture(bgSampler, bgImageView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

bool LoadingScreen::loadImage(const std::string& path) {
    if (!vkCtx) {
        LOG_WARNING("No VkContext for loading screen image");
        return false;
    }

    if (bgImage) {
        VkDevice device = vkCtx->getDevice();
        vkDeviceWaitIdle(device);
        bgSampler = VK_NULL_HANDLE;
        if (bgImageView) { vkDestroyImageView(device, bgImageView, nullptr); bgImageView = VK_NULL_HANDLE; }
        if (bgImage) { vkDestroyImage(device, bgImage, nullptr); bgImage = VK_NULL_HANDLE; }
        if (bgMemory) { vkFreeMemory(device, bgMemory, nullptr); bgMemory = VK_NULL_HANDLE; }
        bgDescriptorSet = VK_NULL_HANDLE;
    }

    std::vector<uint8_t> owned;
    const unsigned char* pixels = nullptr;

    if (!path.empty() && assetManager_ && assetManager_->isInitialized()) {
        pipeline::BLPImage blp = assetManager_->loadTexture(path);
        if (blp.isValid() && !blp.data.empty()) {
            owned = std::move(blp.data);
            imageWidth = blp.width;
            imageHeight = blp.height;
            pixels = owned.data();
            LOG_INFO("Loaded loading screen BLP: ", path, " ", imageWidth, "x", imageHeight);
        }
    }

    if (!pixels && !path.empty()) {
        int channels = 0;
        stbi_set_flip_vertically_on_load(false);
        unsigned char* data = stbi_load(path.c_str(), &imageWidth, &imageHeight, &channels, 4);
        if (data) {
            owned.assign(data, data + static_cast<size_t>(imageWidth) * imageHeight * 4);
            stbi_image_free(data);
            pixels = owned.data();
            LOG_INFO("Loaded loading screen image: ", path, " ", imageWidth, "x", imageHeight);
        }
    }

    if (!pixels) {
        // Obsidian procedural backdrop — never leave enter-world as a blank flash.
        imageWidth = 256;
        imageHeight = 256;
        owned.resize(static_cast<size_t>(imageWidth) * imageHeight * 4);
        for (int y = 0; y < imageHeight; ++y) {
            for (int x = 0; x < imageWidth; ++x) {
                float t = static_cast<float>(y) / static_cast<float>(imageHeight - 1);
                size_t i = (static_cast<size_t>(y) * imageWidth + x) * 4;
                owned[i + 0] = static_cast<uint8_t>(12 + t * 28);
                owned[i + 1] = static_cast<uint8_t>(8 + t * 10);
                owned[i + 2] = static_cast<uint8_t>(28 + t * 48);
                owned[i + 3] = 255;
            }
        }
        pixels = owned.data();
        if (!path.empty()) {
            LOG_WARNING("Failed to load loading screen image: ", path, " — using procedural backdrop");
        }
    }

    return uploadRgba(pixels, imageWidth, imageHeight);
}

void LoadingScreen::renderOverlay() {
    // Draw loading screen content as ImGui overlay within an existing ImGui frame.
    // Caller is responsible for ImGui NewFrame/Render and Vulkan frame management.
    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##LoadingScreenOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (bgDescriptorSet) {
        ImGui::GetWindowDrawList()->AddImage(
            reinterpret_cast<ImTextureID>(bgDescriptorSet),
            ImVec2(0, 0), ImVec2(screenW, screenH));
    }

    // Zone name header
    if (!zoneName.empty()) {
        ImFont* font = ImGui::GetFont();
        float zoneTextSize = 24.0f;
        ImVec2 zoneSize = font->CalcTextSizeA(zoneTextSize, FLT_MAX, 0.0f, zoneName.c_str());
        float zoneX = (screenW - zoneSize.x) * 0.5f;
        float zoneY = screenH * 0.06f - 44.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(font, zoneTextSize, ImVec2(zoneX + 2.0f, zoneY + 2.0f),
                    IM_COL32(0, 0, 0, 200), zoneName.c_str());
        dl->AddText(font, zoneTextSize, ImVec2(zoneX, zoneY),
                    IM_COL32(255, 220, 120, 255), zoneName.c_str());
    }

    // Progress bar
    {
        const float barWidthFrac = 0.6f;
        const float barHeight = 6.0f;
        const float barY = screenH * 0.06f;
        float barX = screenW * (0.5f - barWidthFrac * 0.5f);
        float barW = screenW * barWidthFrac;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barHeight),
            IM_COL32(25, 25, 25, 200), 2.0f);
        if (loadProgress > 0.001f) {
            drawList->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * loadProgress, barY + barHeight),
                IM_COL32(199, 156, 33, 255), 2.0f);
        }
        drawList->AddRect(ImVec2(barX - 1, barY - 1), ImVec2(barX + barW + 1, barY + barHeight + 1),
            IM_COL32(140, 110, 25, 255), 2.0f);
    }

    // Percentage text
    {
        char pctBuf[32];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", static_cast<int>(loadProgress * 100.0f));
        float textY = screenH * 0.06f - 20.0f;
        ImVec2 pctSize = ImGui::CalcTextSize(pctBuf);
        ImGui::SetCursorPos(ImVec2((screenW - pctSize.x) * 0.5f, textY));
        ImGui::TextColored(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), "%s", pctBuf);
    }

    // Status text
    {
        float statusY = screenH * 0.06f + 14.0f;
        ImVec2 statusSize = ImGui::CalcTextSize(statusText.c_str());
        ImGui::SetCursorPos(ImVec2((screenW - statusSize.x) * 0.5f, statusY));
        ImGui::TextColored(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), "%s", statusText.c_str());
    }

    ImGui::End();
}

void LoadingScreen::render() {
    // If a frame is already in progress (e.g. called from a UI callback),
    // end it before starting our own
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx && ctx->FrameCount >= 0 && ctx->WithinFrameScope) {
        ImGui::EndFrame();
    }

    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Invisible fullscreen window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##LoadingScreen", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Draw background image
    if (bgDescriptorSet) {
        ImGui::GetWindowDrawList()->AddImage(
            reinterpret_cast<ImTextureID>(bgDescriptorSet),
            ImVec2(0, 0), ImVec2(screenW, screenH));
    }

    // Zone name header (large text centered above progress bar)
    if (!zoneName.empty()) {
        ImFont* font = ImGui::GetFont();
        float zoneTextSize = 24.0f;
        ImVec2 zoneSize = font->CalcTextSizeA(zoneTextSize, FLT_MAX, 0.0f, zoneName.c_str());
        float zoneX = (screenW - zoneSize.x) * 0.5f;
        float zoneY = screenH * 0.06f - 44.0f;  // above percentage text
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Drop shadow
        dl->AddText(font, zoneTextSize, ImVec2(zoneX + 2.0f, zoneY + 2.0f),
                    IM_COL32(0, 0, 0, 200), zoneName.c_str());
        // Gold text
        dl->AddText(font, zoneTextSize, ImVec2(zoneX, zoneY),
                    IM_COL32(255, 220, 120, 255), zoneName.c_str());
    }

    // Progress bar (top of screen)
    {
        const float barWidthFrac = 0.6f;
        const float barHeight = 6.0f;
        const float barY = screenH * 0.06f;
        float barX = screenW * (0.5f - barWidthFrac * 0.5f);
        float barW = screenW * barWidthFrac;

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(
            ImVec2(barX, barY),
            ImVec2(barX + barW, barY + barHeight),
            IM_COL32(25, 25, 25, 200), 2.0f);

        // Fill (gold)
        if (loadProgress > 0.001f) {
            drawList->AddRectFilled(
                ImVec2(barX, barY),
                ImVec2(barX + barW * loadProgress, barY + barHeight),
                IM_COL32(199, 156, 33, 255), 2.0f);
        }

        // Border
        drawList->AddRect(
            ImVec2(barX - 1, barY - 1),
            ImVec2(barX + barW + 1, barY + barHeight + 1),
            IM_COL32(140, 110, 25, 255), 2.0f);
    }

    // Percentage text above bar
    {
        char pctBuf[32];
        snprintf(pctBuf, sizeof(pctBuf), "%d%%", static_cast<int>(loadProgress * 100.0f));
        float barCenterY = screenH * 0.06f;
        float textY = barCenterY - 20.0f;

        ImVec2 pctSize = ImGui::CalcTextSize(pctBuf);
        ImGui::SetCursorPos(ImVec2((screenW - pctSize.x) * 0.5f, textY));
        ImGui::TextColored(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), "%s", pctBuf);
    }

    // Status text below bar
    {
        float statusY = screenH * 0.06f + 14.0f;
        ImVec2 statusSize = ImGui::CalcTextSize(statusText.c_str());
        ImGui::SetCursorPos(ImVec2((screenW - statusSize.x) * 0.5f, statusY));
        ImGui::TextColored(ImVec4(0.0f, 0.0f, 0.0f, 1.0f), "%s", statusText.c_str());
    }

    ImGui::End();
    ImGui::Render();

    // Submit the frame to Vulkan (loading screen runs outside the main render loop)
    if (vkCtx) {
        // Handle window resize: recreate swapchain before acquiring an image
        if (vkCtx->isSwapchainDirty() && sdlWindow) {
            int w = 0, h = 0;
            SDL_GetWindowSize(sdlWindow, &w, &h);
            if (w > 0 && h > 0) {
                (void)vkCtx->recreateSwapchain(w, h);
            }
        }

        uint32_t imageIndex = 0;
        VkCommandBuffer cmd = vkCtx->beginFrame(imageIndex);
        if (cmd != VK_NULL_HANDLE) {
            // Begin render pass
            // The UI draws in the overlay pass, which is what ImGui's pipelines
            // are built for: single-sampled and colour only. Drawing it in the
            // scene pass instead put a 1x pipeline inside an 8x pass, which the
            // validation layers reject and the driver renders as it pleases.
            // This variant clears, since there is no scene underneath here.
            VkRenderPassBeginInfo rpInfo{};
            rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpInfo.renderPass = vkCtx->getOverlayClearRenderPass();
            rpInfo.framebuffer = vkCtx->getOverlayFramebuffers()[imageIndex];
            rpInfo.renderArea.offset = {0, 0};
            rpInfo.renderArea.extent = vkCtx->getSwapchainExtent();

            VkClearValue clearValues[1]{};
            clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            rpInfo.clearValueCount = 1;
            rpInfo.pClearValues = clearValues;

            const bool overlayReady =
                rpInfo.renderPass != VK_NULL_HANDLE &&
                imageIndex < vkCtx->getOverlayFramebuffers().size();
            if (overlayReady) {
                vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
                vkCmdEndRenderPass(cmd);
            }

            vkCtx->endFrame(cmd, imageIndex);
        }
    }
}

} // namespace rendering
} // namespace wowee
