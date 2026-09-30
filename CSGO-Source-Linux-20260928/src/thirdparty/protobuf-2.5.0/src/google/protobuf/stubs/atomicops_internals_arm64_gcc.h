// AArch64 implementation of protobuf 2.5's atomic operations using compiler
// atomics. AtomicWord and Atomic64 are the same LP64 type on this platform.
#ifndef GOOGLE_PROTOBUF_ATOMICOPS_INTERNALS_ARM64_GCC_H_
#define GOOGLE_PROTOBUF_ATOMICOPS_INTERNALS_ARM64_GCC_H_

namespace google { namespace protobuf { namespace internal {

inline void MemoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

#define PROTOBUF_ARM64_ATOMICS(T) \
inline T NoBarrier_CompareAndSwap(volatile T* p, T old_value, T value) { \
  __atomic_compare_exchange_n(p, &old_value, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED); return old_value; \
} \
inline T Acquire_CompareAndSwap(volatile T* p, T old_value, T value) { \
  __atomic_compare_exchange_n(p, &old_value, value, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE); return old_value; \
} \
inline T Release_CompareAndSwap(volatile T* p, T old_value, T value) { \
  __atomic_compare_exchange_n(p, &old_value, value, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED); return old_value; \
} \
inline T NoBarrier_AtomicExchange(volatile T* p, T value) { return __atomic_exchange_n(p, value, __ATOMIC_RELAXED); } \
inline T NoBarrier_AtomicIncrement(volatile T* p, T value) { return __atomic_add_fetch(p, value, __ATOMIC_RELAXED); } \
inline T Barrier_AtomicIncrement(volatile T* p, T value) { return __atomic_add_fetch(p, value, __ATOMIC_SEQ_CST); } \
inline void NoBarrier_Store(volatile T* p, T value) { __atomic_store_n(p, value, __ATOMIC_RELAXED); } \
inline void Acquire_Store(volatile T* p, T value) { __atomic_store_n(p, value, __ATOMIC_RELAXED); MemoryBarrier(); } \
inline void Release_Store(volatile T* p, T value) { __atomic_store_n(p, value, __ATOMIC_RELEASE); } \
inline T NoBarrier_Load(volatile const T* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); } \
inline T Acquire_Load(volatile const T* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); } \
inline T Release_Load(volatile const T* p) { MemoryBarrier(); return __atomic_load_n(p, __ATOMIC_RELAXED); }

PROTOBUF_ARM64_ATOMICS(Atomic32)
PROTOBUF_ARM64_ATOMICS(Atomic64)
#undef PROTOBUF_ARM64_ATOMICS

}}}
#endif
