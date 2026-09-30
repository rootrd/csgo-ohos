#include "d3d11_device.h"
#include "d3d11_query.h"
#include "../dxvk/dxvk_winehua_trace.h"
#include "../dxvk/dxvk_winehua_submit_stats.h"

namespace dxvk {
  
  D3D11Query::D3D11Query(
          D3D11Device*       device,
    const D3D11_QUERY_DESC1& desc)
  : D3D11DeviceChild<ID3D11Query1>(device),
    m_desc(desc),
    m_state(D3D11_VK_QUERY_INITIAL),
    m_d3d10(this) {
    winehuaQueryTrace(str::format(
      "d3d11-query-create query=", this,
      " type=", uint32_t(m_desc.Query),
      " flags=", m_desc.MiscFlags));

    Rc<DxvkDevice> dxvkDevice = m_parent->GetDXVKDevice();

    switch (m_desc.Query) {
      case D3D11_QUERY_EVENT:
        m_completionSignal = new sync::Fence(0);
        winehuaQueryTrace(str::format(
          "d3d11-query-event backend=submit-completion query=", this));
        break;
        
      case D3D11_QUERY_OCCLUSION:
        m_query[0] = dxvkDevice->createGpuQuery(
          VK_QUERY_TYPE_OCCLUSION,
          VK_QUERY_CONTROL_PRECISE_BIT, 0);
        break;
      
      case D3D11_QUERY_OCCLUSION_PREDICATE:
        m_query[0] = dxvkDevice->createGpuQuery(
          VK_QUERY_TYPE_OCCLUSION, 0, 0);
        break;
        
      case D3D11_QUERY_TIMESTAMP:
        m_query[0] = dxvkDevice->createGpuQuery(
          VK_QUERY_TYPE_TIMESTAMP, 0, 0);
        break;
      
      case D3D11_QUERY_TIMESTAMP_DISJOINT:
        for (uint32_t i = 0; i < 2; i++) {
          m_query[i] = dxvkDevice->createGpuQuery(
            VK_QUERY_TYPE_TIMESTAMP, 0, 0);
        }
        break;
      
      case D3D11_QUERY_PIPELINE_STATISTICS:
        if (dxvkDevice->features().core.features.pipelineStatisticsQuery) {
          m_query[0] = dxvkDevice->createGpuQuery(
            VK_QUERY_TYPE_PIPELINE_STATISTICS, 0, 0);
        } else {
          static std::atomic<bool> s_warnedPipelineStatistics { false };
          if (!s_warnedPipelineStatistics.exchange(true))
            Logger::warn("WineHua: pipeline statistics queries are unsupported by the Vulkan device; returning zero statistics");
        }
        break;
      
      case D3D11_QUERY_SO_STATISTICS:
      case D3D11_QUERY_SO_STATISTICS_STREAM0:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
      case D3D11_QUERY_SO_STATISTICS_STREAM1:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
      case D3D11_QUERY_SO_STATISTICS_STREAM2:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
      case D3D11_QUERY_SO_STATISTICS_STREAM3:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
        if (dxvkDevice->features().extTransformFeedback.transformFeedback) {
          uint32_t stream = 0;
          switch (m_desc.Query) {
            case D3D11_QUERY_SO_STATISTICS_STREAM1:
            case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1: stream = 1; break;
            case D3D11_QUERY_SO_STATISTICS_STREAM2:
            case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2: stream = 2; break;
            case D3D11_QUERY_SO_STATISTICS_STREAM3:
            case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: stream = 3; break;
            default: break;
          }
          m_query[0] = dxvkDevice->createGpuQuery(
            VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT, 0, stream);
        } else {
          static std::atomic<bool> s_warnedTransformFeedback { false };
          if (!s_warnedTransformFeedback.exchange(true))
            Logger::warn("WineHua: transform-feedback queries are unsupported by the Vulkan device; returning zero statistics");
        }
        break;
      
      default:
        throw DxvkError(str::format("D3D11: Unhandled query type: ", desc.Query));
    }
  }
  
  
  D3D11Query::~D3D11Query() {

  }
  
    
  HRESULT STDMETHODCALLTYPE D3D11Query::QueryInterface(REFIID  riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;
    
    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(ID3D11DeviceChild)
     || riid == __uuidof(ID3D11Asynchronous)
     || riid == __uuidof(ID3D11Query)
     || riid == __uuidof(ID3D11Query1)) {
      *ppvObject = ref(this);
      return S_OK;
    }
    
    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(ID3D10DeviceChild)
     || riid == __uuidof(ID3D10Asynchronous)
     || riid == __uuidof(ID3D10Query)) {
      *ppvObject = ref(&m_d3d10);
      return S_OK;
    }
    
    if (m_desc.Query == D3D11_QUERY_OCCLUSION_PREDICATE) {
      if (riid == __uuidof(ID3D11Predicate)) {
        *ppvObject = AsPredicate(ref(this));
        return S_OK;
      }

      if (riid == __uuidof(ID3D10Predicate)) {
        *ppvObject = ref(&m_d3d10);
        return S_OK;
      }
    }
    
    Logger::warn("D3D11Query: Unknown interface query");
    Logger::warn(str::format(riid));
    return E_NOINTERFACE;
  }
  
  
  UINT STDMETHODCALLTYPE D3D11Query::GetDataSize() {
    switch (m_desc.Query) {
      case D3D11_QUERY_EVENT:
        return sizeof(BOOL);
      
      case D3D11_QUERY_OCCLUSION:
        return sizeof(UINT64);
      
      case D3D11_QUERY_TIMESTAMP:
        return sizeof(UINT64);
      
      case D3D11_QUERY_TIMESTAMP_DISJOINT:
        return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
      
      case D3D11_QUERY_PIPELINE_STATISTICS:
        return sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS);
      
      case D3D11_QUERY_OCCLUSION_PREDICATE:
        return sizeof(BOOL);
      
      case D3D11_QUERY_SO_STATISTICS:
      case D3D11_QUERY_SO_STATISTICS_STREAM0:
      case D3D11_QUERY_SO_STATISTICS_STREAM1:
      case D3D11_QUERY_SO_STATISTICS_STREAM2:
      case D3D11_QUERY_SO_STATISTICS_STREAM3:
        return sizeof(D3D11_QUERY_DATA_SO_STATISTICS);
      
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
      case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3:
        return sizeof(BOOL);
    }
    
    Logger::err("D3D11Query: Failed to query data size");
    return 0;
  }
  
    
  void STDMETHODCALLTYPE D3D11Query::GetDesc(D3D11_QUERY_DESC* pDesc) {
    pDesc->Query     = m_desc.Query;
    pDesc->MiscFlags = m_desc.MiscFlags;
  }
  
  
  void STDMETHODCALLTYPE D3D11Query::GetDesc1(D3D11_QUERY_DESC1* pDesc) {
    *pDesc = m_desc;
  }
  
  
  void D3D11Query::Begin(DxvkContext* ctx) {
    winehuaQueryCounters().begin.fetch_add(1, std::memory_order_relaxed);
    winehuaFlowTrace(str::format(
      "d3d11-query-cmd-begin type=", uint32_t(m_desc.Query),
      " query=", this));
    winehuaQueryTrace(str::format(
      "d3d11-query-cmd-begin query=", this,
      " type=", uint32_t(m_desc.Query)));

    switch (m_desc.Query) {
      case D3D11_QUERY_EVENT:
      case D3D11_QUERY_TIMESTAMP:
        break;

      case D3D11_QUERY_TIMESTAMP_DISJOINT:
        ctx->writeTimestamp(m_query[1]);
        break;
      
      default:
        if (m_query[0] != nullptr)
          ctx->beginQuery(m_query[0]);
    }
  }
  
  
  void D3D11Query::End(DxvkContext* ctx) {
    winehuaQueryCounters().end.fetch_add(1, std::memory_order_relaxed);
    winehuaFlowTrace(str::format(
      "d3d11-query-cmd-end begin type=", uint32_t(m_desc.Query),
      " query=", this));
    winehuaQueryTrace(str::format(
      "d3d11-query-cmd-end begin query=", this,
      " type=", uint32_t(m_desc.Query),
      " reset=", m_resetCtr.load(std::memory_order_relaxed)));

    switch (m_desc.Query) {
      case D3D11_QUERY_EVENT:
        ctx->signal(m_completionSignal,
          m_completionValue.load(std::memory_order_acquire));
        break;
      
      case D3D11_QUERY_TIMESTAMP:
      case D3D11_QUERY_TIMESTAMP_DISJOINT:
        ctx->writeTimestamp(m_query[0]);
        break;
      
      default:
        if (m_query[0] != nullptr)
          ctx->endQuery(m_query[0]);
    }

    m_resetCtr.fetch_sub(1, std::memory_order_release);

    winehuaQueryTrace(str::format(
      "d3d11-query-cmd-end done query=", this,
      " reset=", m_resetCtr.load(std::memory_order_relaxed)));
    winehuaFlowTrace(str::format(
      "d3d11-query-cmd-end done type=", uint32_t(m_desc.Query),
      " query=", this,
      " reset=", m_resetCtr.load(std::memory_order_relaxed)));
  }
  
  
  bool STDMETHODCALLTYPE D3D11Query::DoBegin() {
    if (!IsScoped() || m_state == D3D11_VK_QUERY_BEGUN)
      return false;

    m_state = D3D11_VK_QUERY_BEGUN;
    winehuaQueryTrace(str::format(
      "d3d11-query-begin query=", this,
      " type=", uint32_t(m_desc.Query)));
    return true;
  }

  bool STDMETHODCALLTYPE D3D11Query::DoEnd() {
    // Apparently the D3D11 runtime implicitly begins the query
    // if it is in the wrong state at the time End is called, so
    // let the caller react to it instead of just failing here.
    bool result = m_state == D3D11_VK_QUERY_BEGUN || !IsScoped();

    m_state = D3D11_VK_QUERY_ENDED;
    m_resetCtr.fetch_add(1, std::memory_order_acquire);
    m_completionValue.fetch_add(1, std::memory_order_release);
    winehuaQueryTrace(str::format(
      "d3d11-query-end query=", this,
      " type=", uint32_t(m_desc.Query),
      " reset=", m_resetCtr.load(std::memory_order_relaxed)));
    return result;
  }


  HRESULT STDMETHODCALLTYPE D3D11Query::GetData(
          void*                             pData,
          UINT                              GetDataFlags) {
    winehuaQueryTrace(str::format(
      "d3d11-query-get-data query=", this,
      " type=", uint32_t(m_desc.Query),
      " state=", uint32_t(m_state),
      " reset=", m_resetCtr.load(std::memory_order_relaxed)));

    if (m_state != D3D11_VK_QUERY_ENDED)
      return DXGI_ERROR_INVALID_CALL;

    winehuaQueryCounters().getData.fetch_add(1, std::memory_order_relaxed);
    if (m_resetCtr != 0u)
    {
      winehuaQueryCounters().getDataPending.fetch_add(1, std::memory_order_relaxed);
      return S_FALSE;
    }

    if (m_desc.Query == D3D11_QUERY_EVENT) {
      bool signaled = false;
      uint32_t statusValue = uint32_t(DxvkGpuEventStatus::Pending);

      const uint64_t target = m_completionValue.load(std::memory_order_acquire);
      const uint64_t completed = m_completionSignal->value();
      signaled = completed >= target;
      statusValue = signaled
        ? uint32_t(DxvkGpuEventStatus::Signaled)
        : uint32_t(DxvkGpuEventStatus::Pending);

      if (m_traceEventStatus.exchange(statusValue, std::memory_order_relaxed) != statusValue)
        winehuaFlowTrace(str::format(
          "d3d11-event-status query=", this,
          " backend=submit-completion",
          " status=", statusValue));

      if (pData != nullptr)
        *static_cast<BOOL*>(pData) = signaled;
      
      return signaled ? S_OK : S_FALSE;
    } else {
      std::array<DxvkQueryData, MaxGpuQueries> queryData = { };
      
      for (uint32_t i = 0; i < MaxGpuQueries && m_query[i] != nullptr; i++) {
        DxvkGpuQueryStatus status = m_query[i]->getData(queryData[i]);

        if (status == DxvkGpuQueryStatus::Invalid
         || status == DxvkGpuQueryStatus::Failed)
          return DXGI_ERROR_INVALID_CALL;
        
        if (status == DxvkGpuQueryStatus::Pending)
          return S_FALSE;
      }
      
      if (pData == nullptr)
        return S_OK;
      
      switch (m_desc.Query) {
        case D3D11_QUERY_OCCLUSION:
          *static_cast<UINT64*>(pData) = queryData[0].occlusion.samplesPassed;
          return S_OK;
        
        case D3D11_QUERY_OCCLUSION_PREDICATE:
          *static_cast<BOOL*>(pData) = queryData[0].occlusion.samplesPassed != 0;
          return S_OK;
        
        case D3D11_QUERY_TIMESTAMP:
          *static_cast<UINT64*>(pData) = queryData[0].timestamp.time;
          return S_OK;
        
        case D3D11_QUERY_TIMESTAMP_DISJOINT: {
          auto data = static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(pData);
          data->Frequency = GetTimestampQueryFrequency();
          data->Disjoint  = queryData[0].timestamp.time < queryData[1].timestamp.time;
        } return S_OK;
        
        case D3D11_QUERY_PIPELINE_STATISTICS: {
          auto data = static_cast<D3D11_QUERY_DATA_PIPELINE_STATISTICS*>(pData);
          data->IAVertices    = queryData[0].statistic.iaVertices;
          data->IAPrimitives  = queryData[0].statistic.iaPrimitives;
          data->VSInvocations = queryData[0].statistic.vsInvocations;
          data->GSInvocations = queryData[0].statistic.gsInvocations;
          data->GSPrimitives  = queryData[0].statistic.gsPrimitives;
          data->CInvocations  = queryData[0].statistic.clipInvocations;
          data->CPrimitives   = queryData[0].statistic.clipPrimitives;
          data->PSInvocations = queryData[0].statistic.fsInvocations;
          data->HSInvocations = queryData[0].statistic.tcsPatches;
          data->DSInvocations = queryData[0].statistic.tesInvocations;
          data->CSInvocations = queryData[0].statistic.csInvocations;
        } return S_OK;

        case D3D11_QUERY_SO_STATISTICS:
        case D3D11_QUERY_SO_STATISTICS_STREAM0:
        case D3D11_QUERY_SO_STATISTICS_STREAM1:
        case D3D11_QUERY_SO_STATISTICS_STREAM2:
        case D3D11_QUERY_SO_STATISTICS_STREAM3: {
          auto data = static_cast<D3D11_QUERY_DATA_SO_STATISTICS*>(pData);
          data->NumPrimitivesWritten    = queryData[0].xfbStream.primitivesWritten;
          data->PrimitivesStorageNeeded = queryData[0].xfbStream.primitivesNeeded;
        } return S_OK;
          
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2:
        case D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3: {
          auto data = static_cast<BOOL*>(pData);
          *data = queryData[0].xfbStream.primitivesNeeded
                > queryData[0].xfbStream.primitivesWritten;
        } return S_OK;

        default:
          Logger::err(str::format("D3D11: Unhandled query type in GetData: ", m_desc.Query));
          return E_INVALIDARG;
      }
    }
  }
  
  
  UINT64 D3D11Query::GetTimestampQueryFrequency() const {
    Rc<DxvkDevice>  device  = m_parent->GetDXVKDevice();
    Rc<DxvkAdapter> adapter = device->adapter();

    VkPhysicalDeviceLimits limits = adapter->deviceProperties().limits;
    return uint64_t(1'000'000'000.0f / limits.timestampPeriod);
  }


  HRESULT D3D11Query::ValidateDesc(const D3D11_QUERY_DESC1* pDesc) {
    if (pDesc->Query       >= D3D11_QUERY_PIPELINE_STATISTICS
     && pDesc->ContextType >  D3D11_CONTEXT_TYPE_3D)
      return E_INVALIDARG;
    
    return S_OK;
  }
  
}
