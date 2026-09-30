cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(csgo_vulkan LANGUAGES CXX)
    find_package(SDL3 3.2 CONFIG REQUIRED)
endif()

# NDK/OHOS SDK supply their own headers. Desktop builds use the installed Vulkan SDK's
# headers only; entry points are always resolved through SDL's Vulkan loader.
if(NOT ANDROID AND NOT OHOS)
    find_package(Vulkan REQUIRED)
    # Source's shipped static libraries use libstdc++'s original string ABI.
    add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)
endif()
add_library(csgo_vulkan STATIC vulkan_context.cpp vulkan_resources.cpp vulkan_memory.cpp
    vulkan_upload.cpp vulkan_graphics.cpp vulkan_descriptors.cpp vulkan_draw.cpp vulkan_target.cpp)
target_compile_features(csgo_vulkan PUBLIC cxx_std_17)
target_compile_definitions(csgo_vulkan PUBLIC VK_NO_PROTOTYPES)
target_compile_definitions(csgo_vulkan PRIVATE
    VMA_VULKAN_VERSION=1001000 VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
target_include_directories(csgo_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(csgo_vulkan SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/vma")
target_link_libraries(csgo_vulkan PUBLIC SDL3::SDL3)
if(NOT ANDROID)
    target_include_directories(csgo_vulkan SYSTEM PUBLIC "${Vulkan_INCLUDE_DIRS}")
endif()
target_compile_options(csgo_vulkan PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
set_source_files_properties(vulkan_memory.cpp PROPERTIES COMPILE_OPTIONS "-w")
set_target_properties(csgo_vulkan PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Isolate the Source headers and their platform macros from the Vulkan module's
# public API. The bridge calls the existing vertex-layout code, not a copy of it.
# The Source mesh bridge is used by the desktop graphics probe, not the APK's
# standalone Vulkan probe. Build it only when a consumer explicitly needs it.
add_library(csgo_vulkan_source_sdk INTERFACE)
target_include_directories(csgo_vulkan_source_sdk SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/vma")
target_compile_definitions(csgo_vulkan_source_sdk INTERFACE POSIX _POSIX LINUX _LINUX GNUC COMPILER_GCC
    CSTRIKE15 RAD_TELEMETRY_DISABLED NO_STEAM
    VMA_VULKAN_VERSION=1001000 VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
add_library(csgo_vulkan_source STATIC EXCLUDE_FROM_ALL vulkan_source.cpp vulkan_material.cpp vulkan_device.cpp
    vulkan_texture.cpp vulkan_state.cpp vulkan_queries.cpp vulkan_api.cpp)
target_link_libraries(csgo_vulkan_source PUBLIC csgo_vulkan PRIVATE csgo_vulkan_source_sdk)
target_include_directories(csgo_vulkan_source SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../public")
target_include_directories(csgo_vulkan_source SYSTEM PRIVATE ../../public/tier0 ../../public/tier1)
target_compile_options(csgo_vulkan_source PRIVATE -Wall -Wextra -Wno-unknown-pragmas -Wno-invalid-offsetof -Wno-unused-parameter)
if(ANDROID)
    target_compile_definitions(csgo_vulkan_source PRIVATE ANDROID)
    target_include_directories(csgo_vulkan_source SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime/android/deps/sse2neon")
endif()
set_target_properties(csgo_vulkan_source PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT ANDROID)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(proxy_header "${CMAKE_CURRENT_BINARY_DIR}/vulkan_interface_proxies.h")
    add_custom_command(OUTPUT "${proxy_header}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate-interface-proxies.py" "${proxy_header}"
        DEPENDS generate-interface-proxies.py vulkan_api_unsupported.h ../../public/shaderapi/IShaderDevice.h
        VERBATIM)
    add_library(shaderapivulkan SHARED EXCLUDE_FROM_ALL vulkan_module.cpp "${proxy_header}")
    target_include_directories(shaderapivulkan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" ../../common)
    target_compile_definitions(shaderapivulkan PRIVATE USE_SDL USE_SDL3 USE_NATIVE_VULKAN)
    target_link_libraries(shaderapivulkan PRIVATE csgo_vulkan_source csgo_vulkan_source_sdk
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/public/linux64/tier1_client.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/public/linux64/mathlib_client.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/public/linux64/interfaces_client.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../game/bin/linux64/libtier0_client.so"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../game/bin/linux64/libvstdlib_client.so"
        dl pthread)
    target_link_options(shaderapivulkan PRIVATE "LINKER:--no-undefined" "LINKER:--exclude-libs,ALL")
    set_target_properties(shaderapivulkan PROPERTIES PREFIX "" OUTPUT_NAME shaderapivulkan_client
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    add_library(stdshader_vulkan SHARED EXCLUDE_FROM_ALL native_materials.cpp native_rope.cpp native_screen.cpp native_auxiliary.cpp native_twotexture.cpp native_shadow.cpp
        ../stdshaders/panorama_cshader.cpp ../stdshaders/panoramafancy_cshader.cpp)
    target_include_directories(stdshader_vulkan SYSTEM PRIVATE ../../public ../../public/tier0 ../../public/tier1)
    target_include_directories(stdshader_vulkan SYSTEM PRIVATE .. ../stdshaders ../stdshaders/fxctmp9)
    target_compile_definitions(stdshader_vulkan PRIVATE FAST_MATERIALVAR_ACCESS STDSHADER_DX9_DLL_EXPORT)
    target_link_libraries(stdshader_vulkan PRIVATE csgo_vulkan_source_sdk
        "$<LINK_GROUP:RESCAN,${CMAKE_CURRENT_SOURCE_DIR}/../../lib/public/linux64/shaderlib_client.a,${CMAKE_CURRENT_SOURCE_DIR}/../../lib/public/linux64/tier2_client.a,${CMAKE_CURRENT_SOURCE_DIR}/../../lib/public/linux64/tier1_client.a,${CMAKE_CURRENT_SOURCE_DIR}/../../lib/public/linux64/mathlib_client.a,${CMAKE_CURRENT_SOURCE_DIR}/../../lib/public/linux64/interfaces_client.a>"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../game/bin/linux64/libtier0_client.so"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../game/bin/linux64/libvstdlib_client.so" dl pthread)
    target_link_options(stdshader_vulkan PRIVATE "LINKER:--no-undefined"
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/vulkan_module.exports")
    set_target_properties(stdshader_vulkan PROPERTIES PREFIX "" OUTPUT_NAME stdshader_vulkan_client
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
endif()

add_library(csgo_vulkan_probe STATIC probe/vulkan_probe.cpp)
target_link_libraries(csgo_vulkan_probe PUBLIC csgo_vulkan)
target_compile_options(csgo_vulkan_probe PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
set_target_properties(csgo_vulkan_probe PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT ANDROID)
    add_executable(csgo-vulkan-probe probe/main.cpp)
    target_link_libraries(csgo-vulkan-probe PRIVATE csgo_vulkan_probe)
    add_executable(csgo-vulkan-graphics-probe probe/graphics_probe.cpp)
    target_link_libraries(csgo-vulkan-graphics-probe PRIVATE csgo_vulkan_source csgo_vulkan_source_sdk)
    target_compile_options(csgo-vulkan-graphics-probe PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    add_executable(csgo-vulkan-api-probe probe/api_probe.cpp)
    target_link_libraries(csgo-vulkan-api-probe PRIVATE csgo_vulkan_source csgo_vulkan_source_sdk)
    target_compile_options(csgo-vulkan-api-probe PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
endif()
