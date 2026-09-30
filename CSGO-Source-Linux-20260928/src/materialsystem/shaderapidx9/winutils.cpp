//======Copyright 1996-2006, Valve Corporation, All rights reserved. ======//
//
// winutils.cpp
//
//===========================================================================//

#include "winutils.h"

#ifndef _WIN32

#include "appframework/ilaunchermgr.h"

#if defined(USE_DXVK_NATIVE)
#include <SDL3/SDL.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <climits>

int GetVulkanVideoMemorySize( unsigned vendor, unsigned device )
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if ( vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS ) return 0;
    uint32_t count = 0;
    VkDeviceSize bytes = 0;
    if ( vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS && count )
    {
        std::vector<VkPhysicalDevice> devices(count);
        if ( vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS )
            for ( uint32_t i = 0; i < count; ++i )
            {
                VkPhysicalDeviceProperties properties;
                vkGetPhysicalDeviceProperties(devices[i], &properties);
                if ( properties.vendorID != vendor || properties.deviceID != device ) continue;
                VkPhysicalDeviceMemoryProperties memory;
                vkGetPhysicalDeviceMemoryProperties(devices[i], &memory);
                for ( uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap )
                    if ( memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT )
                        bytes += memory.memoryHeaps[heap].size;
                break;
            }
    }
    vkDestroyInstance(instance, nullptr);
    // This API stores bytes in a signed int; larger unified heaps must saturate.
    return bytes > INT_MAX ? INT_MAX : int(bytes);
}


void GlobalMemoryStatus( MEMORYSTATUS *out )
{
    out->dwLength = sizeof(*out);
    out->dwTotalPhys = size_t(sysconf(_SC_PHYS_PAGES)) * size_t(sysconf(_SC_PAGESIZE));
}
void Sleep( unsigned int milliseconds ) { ThreadSleep(milliseconds); }
bool IsIconic( VD3DHWND window )
{
    if (!window) return true;
    SDL_Window *sdlWindow = static_cast<SDL_Window *>(window);
    const auto flags = SDL_GetWindowFlags(sdlWindow);
#if defined( __OHOS__ )
    return (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))
        || !SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
                                   SDL_PROP_WINDOW_OPENHARMONY_WINDOW_POINTER, nullptr);
#elif defined( ANDROID )
    return (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))
        || !SDL_GetPointerProperty(SDL_GetWindowProperties(sdlWindow),
                                   SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#else
    return (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) || !(flags & SDL_WINDOW_INPUT_FOCUS);
#endif
}
BOOL ClientToScreen( VD3DHWND window, LPPOINT point )
{
    int x = 0, y = 0;
    if (!SDL_GetWindowPosition(static_cast<SDL_Window *>(window), &x, &y)) return false;
    point->x += x; point->y += y;
    return true;
}
BOOL GetClientRect( VD3DHWND window, RECT *rect )
{
    int width = 0, height = 0;
    if (!SDL_GetWindowSizeInPixels(static_cast<SDL_Window *>(window), &width, &height)) return false;
    *rect = {0, 0, width, height};
    return true;
}
void *GetCurrentThread() { return (void *)pthread_self(); }
void SetThreadAffinityMask( void *thread, int mask ) { ThreadSetAffinity((ThreadHandle_t)thread, mask); }
#else
void GlobalMemoryStatus( MEMORYSTATUS *pOut )
{
	//cheese: return 2GB physical
	pOut->dwTotalPhys = (1<<31);
}

void Sleep( unsigned int ms )
{
	DebuggerBreak();
	ThreadSleep( ms );
}

bool IsIconic( VD3DHWND hWnd )
{
	// FIXME for now just act non-minimized all the time
	//DebuggerBreak();
	return false;
}

BOOL ClientToScreen( VD3DHWND hWnd, LPPOINT pPoint )
{
	DebuggerBreak();
	return true;
}

void* GetCurrentThread()
{
	DebuggerBreak();
	return 0;
}

void SetThreadAffinityMask( void *hThread, int nMask )
{
	DebuggerBreak();
}

bool GUID::operator==( const struct _GUID &other ) const
{
	DebuggerBreak();
	return memcmp( this, &other, sizeof( GUID ) ) == 0;
}
#endif
#endif
