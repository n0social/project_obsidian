#pragma once

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

struct SDL_Window;

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;

class LoadingScreen {
public:
    LoadingScreen();
    ~LoadingScreen();

    bool initialize();
    void shutdown();

    void selectRandomImage();

    // Render the loading screen with progress bar and status text (pure ImGui)
    void render();

    // Draw loading screen as ImGui overlay (call within an existing ImGui frame).
    // Used during warmup to overlay loading screen on top of the rendered world.
    void renderOverlay();

    void setProgress(float progress) { loadProgress = progress; }
    void setStatus(const std::string& status) { statusText = status; }
    void setZoneName(const std::string& name) { zoneName = name; }

    // Must be set before initialize() for Vulkan texture upload
    void setVkContext(VkContext* ctx) { vkCtx = ctx; }
    void setSDLWindow(SDL_Window* win) { sdlWindow = win; }
    void setAssetManager(pipeline::AssetManager* am) { assetManager_ = am; }

private:
    bool loadImage(const std::string& path);
    bool uploadRgba(const unsigned char* data, int width, int height);

    VkContext* vkCtx = nullptr;
    SDL_Window* sdlWindow = nullptr;
    pipeline::AssetManager* assetManager_ = nullptr;

    // Vulkan texture for background image
    VkImage bgImage = VK_NULL_HANDLE;
    VkDeviceMemory bgMemory = VK_NULL_HANDLE;
    VkImageView bgImageView = VK_NULL_HANDLE;
    VkSampler bgSampler = VK_NULL_HANDLE;
    VkDescriptorSet bgDescriptorSet = VK_NULL_HANDLE; // ImGui texture handle

    std::vector<std::string> imagePaths;
    int currentImageIndex = 0;

    float loadProgress = 0.0f;
    std::string statusText = "Loading...";
    std::string zoneName;

    int imageWidth = 0;
    int imageHeight = 0;
};

} // namespace rendering
} // namespace wowee
