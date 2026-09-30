#pragma once

#include <cstdint>

namespace dxvk {

  constexpr uint32_t dxbcStageInterfaceComponentCount(
          bool      supportsMaintenance4,
          uint32_t  signatureComponents) {
    return supportsMaintenance4 ? signatureComponents : 4u;
  }

}
