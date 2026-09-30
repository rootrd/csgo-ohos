#include <SDL3/SDL.h>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "appframework/ilaunchermgr.h"
#include "tier0/icommandline.h"

extern void *CreateSDLMgr();

static void Check(bool ok, const char *message)
{
    if (!ok) throw std::runtime_error(message);
}

static std::vector<CCocoaEvent> Events(ILauncherMgr &manager)
{
    manager.PumpWindowsMessageLoop();
    std::vector<CCocoaEvent> result;
    CCocoaEvent events[64];
    for (int count; (count = manager.GetEvents(events, 64)) != 0; )
        result.insert(result.end(), events, events + count);
    return result;
}

static void Push(SDL_Event &event)
{
    Check(SDL_PushEvent(&event), SDL_GetError());
}

int main(int argc, char **argv)
{
    CommandLine()->CreateCmdLine(argc, argv);
    ILauncherMgr &manager = *static_cast<ILauncherMgr *>(CreateSDLMgr());
    try
    {
        Check(manager.Init() == INIT_OK, "SDL3 manager initialization failed");
        SDL_Window *window = static_cast<SDL_Window *>(manager.GetWindowRef());
        Check(window != nullptr, "The renderer needs a window before SetMode");
        Check((SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN)) ==
              (SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN), "Startup must keep a hidden Vulkan window");
        Check(manager.CreateGameWindow("Source SDL3 input validation", true, 640, 480, true), SDL_GetError());
        SDL_HideWindow(window);
        Events(manager);
        uint width = 1280, height = 960;
        manager.RenderedSize(width, height, true);

        // Exercise the real SDL3 event queue and Source's public event ABI.
        SDL_Event event = {};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = SDL_GetWindowID(window);
        event.motion.x = 123; event.motion.y = 157;
        event.motion.xrel = 3; event.motion.yrel = -4;
        Push(event);
        event = {};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.down = true;
        event.key.scancode = SDL_SCANCODE_W;
        event.key.mod = SDL_KMOD_SHIFT;
        Push(event);
        event = {};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.windowID = SDL_GetWindowID(window);
        event.text.text = u8"中";
        Push(event);
        event = {};
        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.y = 1;
        event.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
        Push(event);

        bool motion = false, key = false, textDown = false, textUp = false, wheel = false;
        const std::vector<CCocoaEvent> translatedEvents = Events(manager);
        for (const CCocoaEvent &translated : translatedEvents)
        {
            if (translated.m_EventType == CocoaEvent_MouseMove)
                motion |= translated.m_MousePos[0] == 246 && translated.m_MousePos[1] == 314;
            if (translated.m_EventType == CocoaEvent_KeyDown)
            {
                key |= translated.m_VirtualKeyCode == SDL_SCANCODE_W &&
                       (translated.m_ModifierKeyMask & (1u << eShiftKey));
                textDown |= translated.m_UnicodeKey == 0x4e2d;
            }
            textUp |= translated.m_EventType == CocoaEvent_KeyUp && translated.m_UnicodeKey == 0x4e2d;
            wheel |= translated.m_EventType == CocoaEvent_MouseScroll && translated.m_MousePos[1] == -1;
        }
        if (!(motion && key && textDown && textUp && wheel))
        {
            std::fprintf(stderr, "input: motion=%d key=%d text_down=%d text_up=%d wheel=%d\n",
                         motion, key, textDown, textUp, wheel);
            for (const CCocoaEvent &translated : translatedEvents)
                std::fprintf(stderr, "event: type=%d key=%d unicode=%x mouse=%d,%d\n", int(translated.m_EventType),
                    translated.m_VirtualKeyCode, unsigned(translated.m_UnicodeKey), translated.m_MousePos[0], translated.m_MousePos[1]);
        }
        Check(motion && key && textDown && textUp && wheel, "SDL3 input translation or render-coordinate scaling failed");
        event = {};
        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.y = 0.25f;
        for (int i = 0; i < 4; ++i) Push(event);
        int scroll = 0;
        for (const CCocoaEvent &translated : Events(manager))
            if (translated.m_EventType == CocoaEvent_MouseScroll) scroll += translated.m_MousePos[1];
        Check(scroll == 1, "High-resolution wheel events must accumulate into Source's integer steps");
        int dx = 0, dy = 0;
        manager.GetMouseDelta(dx, dy, false);
        Check(dx == 6 && dy == -8, "Visible pointer deltas must use render coordinates");
        manager.SetMouseVisible(false);
        event = {};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.xrel = 3; event.motion.yrel = -4;
        Push(event);
        Events(manager);
        manager.GetMouseDelta(dx, dy, false);
        Check(dx == 3 && dy == -4, "Relative look deltas must stay in mouse units");
        manager.SetMouseVisible(true);

        event = {};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = true;
        Push(event);
        event = {};
        event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        Push(event);
        event = {};
        event.type = SDL_EVENT_MOUSE_MOTION;
        Push(event);
        bool deactivated = false, released = false;
        for (const CCocoaEvent &translated : Events(manager))
        {
            deactivated |= translated.m_EventType == CocoaEvent_AppActivate && !translated.m_ModifierKeyMask;
            released |= translated.m_EventType == CocoaEvent_MouseMove && !translated.m_MouseButtonFlags;
        }
        Check(deactivated && released, "Focus loss must release held mouse state");

        Check(manager.CreateGameWindow("Source SDL3 platform validation", true, 800, 600, true), SDL_GetError());
        Check(manager.GetWindowRef() == window, "SetMode must preserve window identity");
        SDL_SyncWindow(window);
        int w = 0, h = 0;
        Check(SDL_GetWindowSize(window, &w, &h) && w == 800 && h == 600, "Windowed mode size was not applied");
        manager.SetWindowFullScreen(true, 800, 600, true);
        SDL_SyncWindow(window);
        Check(manager.IsWindowFullScreen(), "Desktop fullscreen failed");
        manager.SetWindowFullScreen(false, 640, 480, true);
        SDL_SyncWindow(window);
        Check(!manager.IsWindowFullScreen() && SDL_GetWindowSize(window, &w, &h) && w == 640 && h == 480,
              "Returning from fullscreen must restore the requested window size");
        uint refresh = 0;
        manager.GetNativeDisplayInfo(manager.GetActiveDisplayIndex(), width, height, refresh);
        Check(width && height, "Display index to SDL3 display ID translation failed");

        std::ifstream maps("/proc/self/maps");
        std::string line;
        bool sdl3 = false;
        while (std::getline(maps, line))
        {
            sdl3 |= line.find("libSDL3.so") != std::string::npos;
            Check(line.find("libSDL2") == std::string::npos && line.find("libtogl") == std::string::npos,
                  "SDL2/ToGL was loaded into the SDL3 process");
        }
        Check(sdl3, "SDL3 library is missing from process mappings");
        manager.Shutdown();
        std::puts("SDL3_PLATFORM_PASS: startup window, keyboard/UTF-8, pointer scaling, relative look, wheel, focus release, mode changes and single SDL ABI");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "SDL3_PLATFORM_FAIL: %s\n", error.what());
        manager.Shutdown();
        return 1;
    }
}
