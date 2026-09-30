#pragma once

#include "vulkan_dispatch.h"
#include <stdexcept>
#include <vector>

// Linux, Android and the standalone framework share the SDL3 window ABI.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace sourcevk::platform {
inline bool loadVulkan() {
    return SDL_Vulkan_LoadLibrary(nullptr);
}
inline std::vector<const char*> instanceExtensions(SDL_Window* window) {
    unsigned count = 0;
    (void)window;
    const auto names = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!names || !count) throw std::runtime_error(SDL_GetError());
    return {names, names + count};
}
inline bool createSurface(SDL_Window* window, VkInstance instance, VkSurfaceKHR* surface) {
    return SDL_Vulkan_CreateSurface(window, instance, nullptr, surface);
}
inline void destroySurface(PFN_vkGetInstanceProcAddr getProc, VkInstance instance, VkSurfaceKHR surface) {
    const auto destroy = reinterpret_cast<PFN_vkDestroySurfaceKHR>(getProc(instance, "vkDestroySurfaceKHR"));
    if (destroy && surface) destroy(instance, surface, nullptr);
}
inline bool pixelSize(SDL_Window* window, int* width, int* height) {
    return SDL_GetWindowSizeInPixels(window, width, height);
}
inline bool windowSize(SDL_Window* window, int* width, int* height) {
    return SDL_GetWindowSize(window, width, height);
}
inline bool resize(SDL_Window* window, int width, int height, bool resizable) {
    return SDL_SetWindowResizable(window, resizable) && SDL_SetWindowSize(window, width, height);
}
inline double time() { return double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency()); }
struct DisplayMode { int width = 0, height = 0, refreshNumerator = 0, refreshDenominator = 1; };
inline std::vector<DisplayMode> displayModes(SDL_Window* window) {
    std::vector<DisplayMode> result;
    int count = 0;
    const auto display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
    auto** modes = SDL_GetFullscreenDisplayModes(display, &count);
    if (!modes) throw std::runtime_error(SDL_GetError());
    try {
        for (int i = 0; i < count; ++i) result.push_back({modes[i]->w, modes[i]->h,
            modes[i]->refresh_rate_numerator, modes[i]->refresh_rate_denominator});
    } catch (...) { SDL_free(modes); throw; }
    SDL_free(modes);
    return result;
}
inline DisplayMode currentMode(SDL_Window* window, bool desktop = false) {
    const auto display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
    const auto* mode = desktop ? SDL_GetDesktopDisplayMode(display) : SDL_GetCurrentDisplayMode(display);
    if (!mode) throw std::runtime_error(SDL_GetError());
    return {mode->w, mode->h, mode->refresh_rate_numerator, mode->refresh_rate_denominator};
}
} // namespace sourcevk::platform
