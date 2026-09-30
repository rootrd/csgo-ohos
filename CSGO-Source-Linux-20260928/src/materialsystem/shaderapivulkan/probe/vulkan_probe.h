#pragma once

#include "vulkan_context.h"

namespace sourcevk {
struct ProbeOptions {
    unsigned seconds = 15;
    bool validation = false;
    bool selfTest = false; // Desktop resize, minimize and Surface replacement.
    std::string outputDirectory;
    LogSink log;
};

// SDL video and the Vulkan window belong to the caller. All device work and
// resource cleanup finish before return. Failures are logged and then thrown.
void runProbe(SDL_Window* window, const ProbeOptions& options);
} // namespace sourcevk
