#include "vulkan_queries.h"
#include "vulkan_internal.h"

#include <algorithm>
#include <map>
#include <vector>

namespace sourcevk {
namespace {
constexpr uint32_t SlotsPerPool=128;
constexpr int Pending=-1,QueryError=-2;
void require(bool value,const char* message) { if(!value)throw std::invalid_argument(message); }
struct QueryPool {
    std::shared_ptr<detail::DeviceState> state;
    VkQueryPool pool=VK_NULL_HANDLE;
    std::vector<uint32_t> free;
    explicit QueryPool(std::shared_ptr<detail::DeviceState> device):state(std::move(device)) {
        free.reserve(SlotsPerPool);
        for(uint32_t i=SlotsPerPool;i>0;--i)free.push_back(i-1);
        VkQueryPoolCreateInfo info {};info.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType=VK_QUERY_TYPE_OCCLUSION;info.queryCount=SlotsPerPool;
        check(state->functions.vkCreateQueryPool(state->device,&info,nullptr,&pool),"vkCreateQueryPool(occlusion)");
    }
    ~QueryPool() { if(pool)state->functions.vkDestroyQueryPool(state->device,pool,nullptr); }
};
struct QuerySample {
    std::shared_ptr<QueryPool> pool;
    uint32_t index=0;
    bool begun=false,ended=false;
    ~QuerySample() { if(pool)pool->free.push_back(index); }
};
struct Query {
    std::vector<std::shared_ptr<QuerySample>> samples;
    uint64_t serial=0;
    bool ended=false,prefixCompleted=false;
    int cached=Pending;
};
}
struct SourceQueries::Impl {
    Context& context;
    size_t maximum;
    Handle next=1;
    std::map<Handle,std::shared_ptr<Query>> queries;
    std::vector<std::shared_ptr<QueryPool>> pools;
    std::shared_ptr<Query> active;
    SourceQueryStatistics counts;
    Impl(Context& owner,size_t limit):context(owner),maximum(limit) {
        require(limit>0 && limit<=65536,"Invalid Source query capacity");
    }
    std::shared_ptr<QuerySample> allocate(const Frame& frame) {
        auto found=std::find_if(pools.begin(),pools.end(),[](const auto& pool) {return !pool->free.empty();});
        std::shared_ptr<QueryPool> pool;
        if(found!=pools.end())pool=*found;
        else {
            require(pools.size()*SlotsPerPool<maximum*4,"Source occlusion query slot budget exhausted");
            pool=std::make_shared<QueryPool>(detail::Access::state(context));pools.push_back(pool);
        }
        auto sample=std::make_shared<QuerySample>();sample->pool=pool;
        sample->index=pool->free.back();pool->free.pop_back();
        detail::Access::retain(context,frame,sample);
        context.vk().vkCmdResetQueryPool(frame.commands,pool->pool,sample->index,1);
        return sample;
    }
};
SourceQueries::SourceQueries(Context& context,size_t maximum):impl_(std::make_unique<Impl>(context,maximum)) {}
SourceQueries::~SourceQueries()=default;
SourceQueries::Handle SourceQueries::create() {
    auto& q=*impl_;
    if(!q.context.capabilities().enabledFeatures.occlusionQueryPrecise)return 0;
    require(q.queries.size()<q.maximum && q.next,"Source occlusion query handle budget exhausted");
    auto query=std::make_shared<Query>();const auto handle=q.next++;
    q.queries.emplace(handle,std::move(query));return handle;
}
void SourceQueries::destroy(Handle handle) {
    auto& q=*impl_;auto found=q.queries.find(handle);
    require(found!=q.queries.end() && found->second!=q.active,"Cannot destroy a foreign or active Source query");
    q.queries.erase(found);
}
bool SourceQueries::active() const { return bool(impl_->active); }
void SourceQueries::invalidate(Handle handle) {
    auto& q=*impl_;auto found=q.queries.find(handle);
    require(found!=q.queries.end() && found->second!=q.active,"Cannot invalidate a foreign or active Source query");
    *found->second=Query();
}
void SourceQueries::begin(Handle handle,const Frame& frame) {
    auto& q=*impl_;auto found=q.queries.find(handle);
    require(found!=q.queries.end() && !q.active,"Invalid or overlapping Source occlusion query");
    q.active=found->second;q.active->samples.clear();q.active->serial=detail::Access::serial(q.context,frame);
    q.active->ended=q.active->prefixCompleted=false;q.active->cached=Pending;++q.counts.begins;
}
void SourceQueries::preparePass(const Frame& frame) {
    auto& q=*impl_;if(!q.active)return;
    require(q.active->serial==detail::Access::serial(q.context,frame),"Source query crossed a frame boundary");
    if(q.active->samples.empty() || q.active->samples.back()->ended)
        q.active->samples.push_back(q.allocate(frame));
}
void SourceQueries::beginDraw(const Frame& frame) {
    auto& q=*impl_;if(!q.active)return;
    require(!q.active->samples.empty(),"Source query was not reset before its render pass");
    auto& sample=*q.active->samples.back();
    require(!sample.ended,"Source query segment already ended");
    if(!sample.begun) {
        q.context.vk().vkCmdBeginQuery(frame.commands,sample.pool->pool,sample.index,VK_QUERY_CONTROL_PRECISE_BIT);
        sample.begun=true;++q.counts.segments;
    }
}
void SourceQueries::endPass(const Frame& frame) {
    auto& q=*impl_;if(!q.active || q.active->samples.empty())return;
    auto& sample=*q.active->samples.back();
    if(sample.begun && !sample.ended) {
        q.context.vk().vkCmdEndQuery(frame.commands,sample.pool->pool,sample.index);sample.ended=true;
    }
}
void SourceQueries::end(Handle handle,const Frame& frame) {
    auto& q=*impl_;auto found=q.queries.find(handle);
    require(found!=q.queries.end() && q.active && q.active==found->second,"Source query end does not match begin");
    endPass(frame);q.active->ended=true;
    if(std::none_of(q.active->samples.begin(),q.active->samples.end(),[](const auto& sample) {return sample->begun;}))
        q.active->cached=0;
    q.active.reset();
}
uint64_t SourceQueries::frameSerial(Handle handle) const {
    auto found=impl_->queries.find(handle);return found==impl_->queries.end()?0:found->second->serial;
}
int SourceQueries::result(Handle handle,bool wait) {
    auto& q=*impl_;auto found=q.queries.find(handle);
    if(found==q.queries.end() || !found->second->serial)return QueryError;
    auto& query=*found->second;
    if(!query.ended)return Pending;
    if(query.cached!=Pending)return query.cached;
    if(!query.prefixCompleted && !q.context.frameComplete(query.serial)) {
        if(!wait)return Pending;
        ++q.counts.forcedWaits;
        if(!q.context.frameComplete(query.serial,true))return Pending;
    }
    uint64_t total=0;
    for(const auto& sample:query.samples)if(sample->begun) {
        uint64_t count=0;
        const auto result=q.context.vk().vkGetQueryPoolResults(q.context.device(),sample->pool->pool,sample->index,1,
            sizeof(count),&count,sizeof(count),VK_QUERY_RESULT_64_BIT);
        if(result==VK_NOT_READY)return Pending;
        check(result,"vkGetQueryPoolResults(occlusion)");total+=count;
    }
    ++q.counts.resultReads;query.cached=int(std::min<uint64_t>(total,0x7fffffffu));return query.cached;
}
void SourceQueries::completedPrefix(const Frame& frame) {
    auto& q=*impl_;const auto serial=detail::Access::serial(q.context,frame);
    for(auto& [handle,query]:q.queries)if(query->serial==serial && query->ended)query->prefixCompleted=true;
    ++q.counts.prefixFlushes;
}
SourceQueryStatistics SourceQueries::statistics() const {
    auto value=impl_->counts;value.liveQueries=impl_->queries.size();value.pools=impl_->pools.size();return value;
}
} // namespace sourcevk
