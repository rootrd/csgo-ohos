#pragma once

#include <vulkan/vulkan_core.h>

namespace dxvk::ohos {

  // A blocking Present may wait for available images, but every finite acquire
  // rechecks window retirement. Rebuild once, never loop on a broken surface.
  template<typename Acquire, typename Recreate, typename Pause>
  VkResult acquireForPresent(bool blocking, Acquire&& acquire, Recreate&& recreate, Pause&& pause) {
    bool rebuilt = false;
    for (;;) {
      const auto status = acquire();
      if (status == VK_TIMEOUT || status == VK_NOT_READY) {
        if (!blocking) return status;
        pause();
        continue;
      }
      if (status == VK_ERROR_OUT_OF_DATE_KHR || status == VK_ERROR_SURFACE_LOST_KHR) {
        if (rebuilt) return status;
        rebuilt = true;
        const auto changed = recreate();
        if (changed != VK_SUCCESS) return changed;
        continue;
      }
      return status;
    }
  }

}
