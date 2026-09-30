#if defined(__linux__) || defined(__ANDROID__)
#include <cstdlib>
namespace {
void* sourceVmaAlignedAllocate(size_t size, size_t alignment) {
    void* memory=nullptr;
    if (alignment<sizeof(void*)) alignment=sizeof(void*);
    return posix_memalign(&memory,alignment,size)==0 ? memory : nullptr;
}
}
// Source's older tcmalloc interposes free/posix_memalign, but not C11
// aligned_alloc. VMA's C++17 default otherwise allocates in libc and frees in
// tcmalloc when loaded by the game. Keep allocation/deallocation on one heap.
#define VMA_SYSTEM_ALIGNED_MALLOC(size, alignment) sourceVmaAlignedAllocate((size), (alignment))
#define VMA_SYSTEM_ALIGNED_FREE(memory) std::free(memory)
#endif
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
