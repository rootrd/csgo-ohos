cmake_minimum_required(VERSION 3.24)
project(csgo_ohos LANGUAGES C CXX)

if(NOT OHOS OR NOT OHOS_ARCH STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Use scripts/build-ohos.sh with the OHOS ARM64 toolchain")
endif()
set(CSGO_OHOS_DEPS "" CACHE PATH "OHOS SDL3 and DXVK install prefix")
find_package(SDL3 CONFIG REQUIRED PATHS "${CSGO_OHOS_DEPS}/lib/cmake/SDL3"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
add_library(dxvk_d3d9 SHARED IMPORTED)
set_target_properties(dxvk_d3d9 PROPERTIES
    IMPORTED_LOCATION "${CSGO_OHOS_DEPS}/lib/libdxvk_d3d9.so"
    INTERFACE_INCLUDE_DIRECTORIES "${CSGO_OHOS_DEPS}/include/dxvk")

set(source_root "${CMAKE_CURRENT_SOURCE_DIR}/../src")
add_subdirectory("${source_root}/materialsystem/shaderapivulkan" shaderapivulkan)
set(CSGO_SOURCE_CONFIG "release" CACHE STRING "Source module configuration")
set(CSGO_BUILD_ID "unknown" CACHE STRING "Source build fingerprint")
set(CSGO_OHOS_PACKAGE "com.csgosource.ohos" CACHE STRING "OHOS application ID")
set(source_libraries "${source_root}/lib/public/androidarm64/${CSGO_SOURCE_CONFIG}")
foreach(module tier0 vstdlib)
    add_library(source_${module} SHARED IMPORTED)
    set_target_properties(source_${module} PROPERTIES
        IMPORTED_LOCATION "${source_libraries}/lib${module}_client.so")
endforeach()
foreach(module tier1 tier2 mathlib interfaces bitmap vtf)
    add_library(source_${module} STATIC IMPORTED)
    set_target_properties(source_${module} PROPERTIES
        IMPORTED_LOCATION "${source_libraries}/${module}_client.a")
endforeach()

add_library(main SHARED native/android_main.cpp native/engine_startup.cpp native/source_assets.cpp)
set(CSGO_OHOS_PROBE "0" CACHE STRING "OHOS 联调：启动即跑 Vulkan 上屏 probe（1=开，0=正常游戏）" FORCE)
target_compile_features(main PRIVATE cxx_std_17)
target_compile_definitions(main PRIVATE
    CSGO_BUILD_ID="${CSGO_BUILD_ID}" CSGO_BUILD_TYPE="${CSGO_SOURCE_CONFIG}"
    CSGO_OHOS_PACKAGE="${CSGO_OHOS_PACKAGE}"
    CSGO_DEV_BUILD=1 # OHOS 联调期恒开：-condebug 控制台日志 + DXVK HUD
    CSGO_OHOS_PROBE=${CSGO_OHOS_PROBE}
    $<$<CONFIG:Debug>:_DEBUG;DEBUG>)
target_compile_options(main PRIVATE -Wall -Wextra -Wpedantic
    -fno-strict-aliasing -fsigned-char -fno-fast-math -ffp-contract=off
    -march=armv8-a -moutline-atomics -Wno-c++11-narrowing -Wno-register)
target_compile_definitions(main PRIVATE POSIX _POSIX LINUX _LINUX __OHOS__ GNUC COMPILER_GCC
    CSTRIKE15 RAD_TELEMETRY_DISABLED USE_DXVK_NATIVE NO_STEAM _DLL_EXT=_client.so _FILE_OFFSET_BITS=64)
target_include_directories(main SYSTEM PRIVATE
    "${source_root}/public" "${source_root}/public/tier0" "${source_root}/public/tier1"
    "${source_root}/common" "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/ohos/deps/sse2neon")
target_link_options(main PRIVATE
    -Wl,--no-undefined -Wl,--build-id=sha1
    -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
target_link_libraries(main PRIVATE
    "$<LINK_GROUP:RESCAN,source_vtf,source_bitmap,source_tier2,source_mathlib,source_tier1,source_interfaces>"
    source_vstdlib source_tier0 SDL3::SDL3 csgo_vulkan_probe dxvk_d3d9
    hilog_ndk.z dl)
set_target_properties(main PROPERTIES CXX_VISIBILITY_PRESET hidden)
