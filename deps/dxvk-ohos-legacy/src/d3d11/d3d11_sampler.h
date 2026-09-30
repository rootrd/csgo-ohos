#pragma once

#include "../dxvk/dxvk_device.h"

#include "../d3d10/d3d10_sampler.h"

#include "d3d11_device_child.h"

namespace dxvk {

  /**
   * \brief Shader-side custom border color parameters
   *
   * The first vector is the requested D3D11 border color. The second
   * vector contains the emulation mode followed by the U/V/W border-axis
   * masks. Mode 0 disables correction, 1 selects point filtering and 2
   * selects linear filtering.
   */
  struct D3D11SamplerEmulationData {
    float borderColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float metadata[4]    = { 0.0f, 0.0f, 0.0f, 0.0f };
  };

  static_assert(sizeof(D3D11SamplerEmulationData) == 32);
  
  class D3D11Device;
  
  class D3D11SamplerState : public D3D11StateObject<ID3D11SamplerState> {
    
  public:
    
    using DescType = D3D11_SAMPLER_DESC;
    
    D3D11SamplerState(
            D3D11Device*        device,
      const D3D11_SAMPLER_DESC& desc);
    ~D3D11SamplerState();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject) final;
    
    void STDMETHODCALLTYPE GetDesc(
            D3D11_SAMPLER_DESC* pDesc) final;
    
    Rc<DxvkSampler> GetDXVKSampler() const {
      return m_sampler;
    }

    const D3D11SamplerEmulationData& GetEmulationData() const {
      return m_emulationData;
    }

    D3D10SamplerState* GetD3D10Iface() {
      return &m_d3d10;
    }
    
    static HRESULT NormalizeDesc(
            D3D11_SAMPLER_DESC* pDesc);
    
  private:
    
    D3D11_SAMPLER_DESC m_desc;
    Rc<DxvkSampler>    m_sampler;
    D3D11SamplerEmulationData m_emulationData;
    D3D10SamplerState  m_d3d10;

    std::atomic<uint32_t> m_refCount = { 0u };

    static bool ValidateAddressMode(
            D3D11_TEXTURE_ADDRESS_MODE  Mode);

    static bool ValidateComparisonFunc(
            D3D11_COMPARISON_FUNC       Comparison);
    
  };
  
}
