# Vulkan Memory Allocator

Unmodified `include/vk_mem_alloc.h` and `LICENSE.txt` from VMA **3.3.0**:
https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/1d8f600fd424278486eade7ed3e877c99f0846b1

SHA-256:

```text
90ce12fc4a2466235a09ae02905dd0c13aee80c1bbf11b331ab61230c2ceb112  vk_mem_alloc.h
8d70e4cf2a9941f6454da71cd60e38484dd3e34810c824760d550e982af6d97a  LICENSE.txt
```

The Vulkan module compiles this once in `vulkan_memory.cpp`, with Vulkan 1.1
and function pointers from SDL's loader. No VMA extension flags are enabled.
