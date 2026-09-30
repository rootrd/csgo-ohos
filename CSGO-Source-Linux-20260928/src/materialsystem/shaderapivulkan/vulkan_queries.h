#pragma once
#include "vulkan_context.h"

namespace sourcevk {
struct SourceQueryStatistics {
    uint64_t begins=0, segments=0, resultReads=0, forcedWaits=0, prefixFlushes=0;
    size_t liveQueries=0, pools=0;
};
// Precise sample counts for Source visibility and HDR luminance histograms.
// Physical slots retire with their frame; logical handles may be reused or
// destroyed immediately without resetting an in-flight GPU query.
class SourceQueries {
public:
    using Handle=uintptr_t;
    SourceQueries(Context& context,size_t maximumQueries=4096);
    ~SourceQueries();
    Handle create();
    void destroy(Handle handle);
    void invalidate(Handle handle);
    void begin(Handle handle,const Frame& frame);
    void end(Handle handle,const Frame& frame);
    bool active() const;
    void preparePass(const Frame& frame); // Outside a Vulkan render pass.
    void beginDraw(const Frame& frame);   // Inside the pass, before geometry.
    void endPass(const Frame& frame);     // End a segment before switching RTs.
    uint64_t frameSerial(Handle handle) const;
    int result(Handle handle,bool wait=false);
    void completedPrefix(const Frame& frame);
    SourceQueryStatistics statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
