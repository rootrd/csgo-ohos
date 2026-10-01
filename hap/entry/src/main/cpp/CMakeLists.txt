cmake_minimum_required(VERSION 3.5.0)
project(csgo_entry)

set(NATIVEAPP_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVEAPP_ROOT_PATH}
                    ${NATIVEAPP_ROOT_PATH}/include)

add_library(entry SHARED napi_init.cpp)
target_link_libraries(entry PUBLIC libace_napi.z.so libhilog_ndk.z.so)
# Align this entry module for 16 KiB pages; SDK runtime alignment is SDK-specific.
set_property(TARGET entry APPEND_STRING PROPERTY LINK_FLAGS " -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")
