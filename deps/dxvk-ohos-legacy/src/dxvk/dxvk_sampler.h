#pragma once

#include "dxvk_resource.h"

namespace dxvk {

  class DxvkDevice;
  
  /**
   * \brief Sampler properties
   */
  struct DxvkSamplerCreateInfo {
    /// Texture filter propertoes
    VkFilter magFilter;
    VkFilter minFilter;
    
    /// Mipmapping properties
    VkSamplerMipmapMode mipmapMode;
    float               mipmapLodBias;
    float               mipmapLodMin;
    float               mipmapLodMax;
    
    /// Anisotropic filtering
    VkBool32 useAnisotropy;
    float    maxAnisotropy;
    
    /// Address modes
    VkSamplerAddressMode addressModeU;
    VkSamplerAddressMode addressModeV;
    VkSamplerAddressMode addressModeW;
    
    /// Compare op for shadow textures
    VkBool32    compareToDepth;
    VkCompareOp compareOp;
    
    /// Texture border color
    VkClearColorValue borderColor;
    
    /// Enables unnormalized coordinates
    VkBool32 usePixelCoord;

    /// Enables non seamless cube map filtering
    VkBool32 nonSeamless;
  };
  
  
  /**
   * \brief Sampler
   * 
   * Manages a sampler object that can be bound to
   * a pipeline. Sampler objects provide parameters
   * for texture lookups within a shader.
   */
  class DxvkSampler : public DxvkResource {
    
  public:
    
    DxvkSampler(
            DxvkDevice*             device,
      const DxvkSamplerCreateInfo&  info);
    ~DxvkSampler();
    
    /**
     * \brief Sampler handle
     * \returns Sampler handle
     */
    VkSampler handle() const {
      return m_sampler;
    }

    const VkSamplerCreateInfo& info() const {
      return m_info;
    }

    const VkClearColorValue& customBorderColor() const {
      return m_customBorderColor;
    }

    VkBool32 compareToDepth() const {
      return m_info.compareEnable;
    }

    VkCompareOp compareOp() const {
      return m_info.compareOp;
    }
    
  private:
    
    Rc<vk::DeviceFn>      m_vkd;
    VkSampler             m_sampler = VK_NULL_HANDLE;
    VkSamplerCreateInfo   m_info = { };
    VkClearColorValue     m_customBorderColor = { };

    static VkBorderColor getBorderColor(
      const Rc<DxvkDevice>&         device,
      const DxvkSamplerCreateInfo&  info);
    
  };
  
}
