#include "vulkan_probe.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

int main(int argc, char** argv) {
    sourcevk::ProbeOptions options;
    SDL_Window* window = nullptr;
    std::ofstream report;
    int result = 0;
    try {
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--validation")) options.validation = true;
            else if (!std::strcmp(argv[i], "--self-test")) options.selfTest = true;
            else if (!std::strcmp(argv[i], "--output") && i + 1 < argc) options.outputDirectory = argv[++i];
            else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) {
                const char* value = argv[++i];
                const auto parsed = std::from_chars(value, value + std::strlen(value), options.seconds);
                if (parsed.ec != std::errc() || *parsed.ptr) throw std::runtime_error("Invalid --seconds");
            } else throw std::runtime_error("Usage: csgo-vulkan-probe [--validation] [--self-test] [--seconds 1..600] [--output DIR]");
        }
        if (!options.outputDirectory.empty()) {
            std::filesystem::create_directories(options.outputDirectory);
            report.open(std::filesystem::path(options.outputDirectory) / "vulkan-probe.log");
            if (!report) throw std::runtime_error("Cannot open Vulkan diagnostic log");
        }
        options.log = [&](const std::string& message) {
            std::fprintf(stderr, "%s\n", message.c_str());
            if (report.is_open()) { report << message << '\n'; report.flush(); }
        };
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("Source Vulkan 1.1 diagnostic", 960, 540,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) throw std::runtime_error(SDL_GetError());
        sourcevk::runProbe(window, options);
    } catch (const std::exception& error) {
        const std::string message = "VK_PROBE_FAILED: " + std::string(error.what());
        if (options.log) options.log(message);
        else std::fprintf(stderr, "%s\n", message.c_str());
        result = 1;
    }
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
