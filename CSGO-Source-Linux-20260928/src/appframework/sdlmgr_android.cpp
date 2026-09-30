// Shared SDL3 window and input owner for Linux and Android renderers.
#include <SDL3/SDL.h>
#include <deque>
#include <atomic>
#include "appframework/ilaunchermgr.h"
#include "tier1/strtools.h"
#include "tier1/convar.h"
#include "tier0/icommandline.h"
#include "inputsystem/mobileinput.h"
#include "tier0/memdbgoff.h"

namespace {
ConVar sdl_displayindex( "sdl_displayindex", "-1", FCVAR_ARCHIVE | FCVAR_HIDDEN, "SDL fullscreen display index." );

static const int64_t kMouseFinger = 0x4d4f5553; // Desktop left button, not a real SDL finger.
class CSDL3Mgr final : public CBaseAppSystem<ILauncherMgr>, public IMobileInputSource
{
    SDL_Window *m_window = nullptr;
    CThreadMutex m_eventMutex;
    std::deque<CCocoaEvent> m_events;
    uint m_width = 0, m_height = 0;
    int m_buttons = 0;
    float m_deltaX = 0, m_deltaY = 0;
    float m_wheelDelta = 0;
    bool m_visible = true, m_forbidGrab = false;
    SDL_Cursor *m_menuCursor = nullptr;
    bool m_mouseFinger = false;
    bool m_watching = false, m_videoInitialized = false;
    bool m_hasFocus = true;
    bool m_androidScreenKeyboardShown = false;
    std::deque<MobileTouchEvent> m_touches;
    MobileTouchMode m_touchMode = MOBILE_TOUCH_DISABLED;
    bool m_uiFingerActive = false, m_touchButton = false;
    int64_t m_uiFinger = 0;
    float m_uiX = 0, m_uiY = 0;
    int m_hardwareButtons = 0;
    std::atomic<bool> m_resetTouchPointer{false};
#if defined(ANDROID)
    SDL_Sensor *m_gyro = nullptr;
    bool m_sensorInitialized = false, m_gyroHave = false, m_gyroWarned = false;
    float m_gyroRateX = 0, m_gyroRateY = 0;
    Uint64 m_gyroRetry = 0, m_gyroRead = 0;
#endif

    SDL_DisplayID PreferredDisplay() const
    {
        int count = 0;
        SDL_DisplayID *displays = SDL_GetDisplays(&count);
        const int index = sdl_displayindex.GetInt();
        SDL_DisplayID display = displays && index >= 0 && index < count ? displays[index]
            : (m_window ? SDL_GetDisplayForWindow(m_window) : SDL_GetPrimaryDisplay());
        SDL_free(displays);
        return display;
    }

#if defined(ANDROID)
    static bool UsableGyroRate( float v ) { return v == v && v > -20.f && v < 20.f; }
    int CurrentGyroOrientation() const
    {
        const SDL_DisplayID display = m_window ? SDL_GetDisplayForWindow(m_window) : SDL_GetPrimaryDisplay();
        return SDL_GetCurrentDisplayOrientation(display) == SDL_ORIENTATION_LANDSCAPE_FLIPPED
            ? MOBILE_GYRO_LANDSCAPE_FLIPPED : MOBILE_GYRO_LANDSCAPE;
    }
    void CloseGyro()
    {
        SDL_Sensor *gyro = nullptr;
        {
            AUTO_LOCK(m_eventMutex);
            gyro = m_gyro;
            m_gyro = nullptr;
            m_gyroHave = false;
        }
        if ( gyro ) SDL_CloseSensor(gyro);
        if ( m_sensorInitialized )
        {
            SDL_QuitSubSystem(SDL_INIT_SENSOR);
            m_sensorInitialized = false;
        }
    }
    void EnsureGyro()
    {
        if ( m_gyro ) return;
        if ( !m_sensorInitialized )
        {
            if ( !SDL_InitSubSystem(SDL_INIT_SENSOR) )
            {
                if ( !m_gyroWarned )
                    Warning("MOBILE_GYRO: sensor init failed: %s\n", SDL_GetError());
                m_gyroWarned = true;
                return;
            }
            m_sensorInitialized = true;
            SDL_SetEventEnabled(SDL_EVENT_SENSOR_UPDATE, true);
        }
        const Uint64 now = SDL_GetTicksNS();
        if ( m_gyroRetry && now < m_gyroRetry ) return;
        m_gyroRetry = now + 2000000000ull;
        int count = 0;
        SDL_SensorID *ids = SDL_GetSensors(&count);
        for ( int i = 0; ids && i < count; ++i )
        {
            if ( SDL_GetSensorTypeForID(ids[i]) != SDL_SENSOR_GYRO ) continue;
            SDL_Sensor *opened = SDL_OpenSensor(ids[i]);
            if ( !opened ) continue;
            {
                AUTO_LOCK(m_eventMutex);
                m_gyro = opened;
            }
            Msg("MOBILE_GYRO: opened %s\n", SDL_GetSensorName(opened));
            break;
        }
        SDL_free(ids);
        if ( !m_gyro && !m_gyroWarned )
        {
            Warning("MOBILE_GYRO: no gyroscope\n");
            m_gyroWarned = true;
        }
    }
#endif

    bool SetWindowMode( bool fullscreen, int width, int height, bool desktopFriendly )
    {
        if ( !m_window ) return false;
#if !defined( ANDROID )
        // SDL3 uses a null fullscreen mode for borderless desktop fullscreen.
        // Display indices in Source's config must be translated to SDL3 IDs.
        const SDL_DisplayID display = PreferredDisplay();
        if ( !fullscreen || SDL_GetDisplayForWindow(m_window) != display )
        {
            if ( !SDL_SetWindowFullscreen(m_window, false) ) return false;
            // SDL3 applies fullscreen/maximize transitions asynchronously.
            // Finish them before choosing the size of a windowed mode.
            if ( !fullscreen && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) && !SDL_RestoreWindow(m_window) )
                return false;
            if ( !SDL_SyncWindow(m_window) ) return false;
        }
        if ( SDL_GetDisplayForWindow(m_window) != display ||
             (SDL_GetWindowFlags(m_window) & SDL_WINDOW_HIDDEN) )
            SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        if ( fullscreen )
        {
            bool exclusive = !desktopFriendly;
            if ( CommandLine()->FindParm("-exclusivefs") ) exclusive = true;
            else if ( CommandLine()->FindParm("-noexclusivefs") ) exclusive = false;
            SDL_DisplayMode mode = {};
            if ( exclusive && !SDL_GetClosestFullscreenDisplayMode(display, width, height, 0, true, &mode) )
                return false;
            if ( !SDL_SetWindowFullscreenMode(m_window, exclusive ? &mode : nullptr) ) return false;
        }
        else if ( width > 0 && height > 0 && !SDL_SetWindowSize(m_window, width, height) )
            return false;
        return SDL_SetWindowFullscreen(m_window, fullscreen) && SDL_SyncWindow(m_window);
#else
        return SDL_SetWindowFullscreen(m_window, fullscreen);
#endif
    }

    void PostTouch( const MobileTouchEvent &event )
    {
        AUTO_LOCK( m_eventMutex );
        if ( m_touches.size() >= 256 )
        {
            // Losing an up event would leave firing/movement held indefinitely.
            m_touches.clear();
            m_touches.push_back({MOBILE_TOUCH_RESET,0,0,0});
        }
        m_touches.push_back(event);
    }

    void WindowToRenderScale( float &x, float &y ) const
    {
        x = y = 1.0f;
        int width = 0, height = 0;
        // SDL mouse coordinates use window units, which may differ from both
        // physical pixels and the D3D backbuffer (including after a reset).
        if ( m_window && SDL_GetWindowSize(m_window, &width, &height)
             && width > 0 && height > 0 && m_width && m_height )
        {
            x = float(m_width) / width;
            y = float(m_height) / height;
        }
    }
    void Post( const CCocoaEvent &event )
    {
        AUTO_LOCK( m_eventMutex );
        m_events.push_back( event );
    }
    static bool Watch( void *context, SDL_Event *event )
    {
        auto *self = static_cast<CSDL3Mgr *>(context);
        // SDL3 sends application lifecycle events only to event watchers.
        if ( event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_FOREGROUND )
        {
            CCocoaEvent translated = {};
            translated.m_EventType = CocoaEvent_AppActivate;
            translated.m_ModifierKeyMask = event->type == SDL_EVENT_DID_ENTER_FOREGROUND;
            self->Post( translated );
            self->PostTouch({MOBILE_TOUCH_RESET,0,0,0});
            self->m_resetTouchPointer=true;
        }
        return true;
    }
    static uint Modifiers( SDL_Keymod mods )
    {
        return ((mods & SDL_KMOD_CAPS) ? 1u << eCapsLockKey : 0)
             | ((mods & SDL_KMOD_SHIFT) ? 1u << eShiftKey : 0)
             | ((mods & SDL_KMOD_CTRL) ? 1u << eControlKey : 0)
             | ((mods & SDL_KMOD_ALT) ? 1u << eAltKey : 0)
             | ((mods & SDL_KMOD_GUI) ? 1u << eCommandKey : 0);
    }
    static int VirtualKey( const SDL_KeyboardEvent &event )
    {
        switch ( event.scancode )
        {
        case SDL_SCANCODE_LSHIFT: return -KEY_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return -KEY_RSHIFT;
        case SDL_SCANCODE_LCTRL: return -KEY_LCONTROL;
        case SDL_SCANCODE_RCTRL: return -KEY_RCONTROL;
        case SDL_SCANCODE_LALT: return -KEY_LALT;
        case SDL_SCANCODE_RALT: return -KEY_RALT;
        case SDL_SCANCODE_LGUI: return -KEY_LWIN;
        case SDL_SCANCODE_RGUI: return -KEY_RWIN;
        case SDL_SCANCODE_CAPSLOCK: return -KEY_CAPSLOCK;
        case SDL_SCANCODE_AC_BACK: return -KEY_ESCAPE;
        default: return int(event.scancode);
        }
    }
    void UpdateGrab()
    {
        if ( !m_window ) return;
        // Desktop menu and touch controls need a normal arrow. The engine hides
        // the pointer and turns on relative mode, which leaves the mobile UI with
        // nothing to click. Android keeps its own touch path.
#if !defined(ANDROID)
        const bool desktopPointer = m_touchMode == MOBILE_TOUCH_UI || m_touchMode == MOBILE_TOUCH_GAME;
#else
        const bool desktopPointer = false;
#endif
        SDL_SetWindowRelativeMouseMode( m_window, m_hasFocus && !m_visible && !m_forbidGrab && !desktopPointer );
        if ( m_visible || !m_hasFocus || desktopPointer )
        {
            if ( desktopPointer )
            {
                if ( !m_menuCursor )
                    m_menuCursor = SDL_CreateSystemCursor( SDL_SYSTEM_CURSOR_DEFAULT );
                if ( m_menuCursor )
                    SDL_SetCursor( m_menuCursor );
            }
            SDL_ShowCursor();
        }
        else
            SDL_HideCursor();
    }
    void UpdateAndroidScreenKeyboard()
    {
#if defined( ANDROID )
        if ( !m_window ) return;
        // Only an explicit request (Panorama text entry focus) shows the IME.
        const bool requested = SDL_GetHintBoolean( SDL_HINT_ENABLE_SCREEN_KEYBOARD, false );
        if ( requested == m_androidScreenKeyboardShown ) return;

        m_androidScreenKeyboardShown = requested;
        if ( requested )
        {
            SDL_StartTextInput( m_window );
        }
        else
        {
            // Hide the soft keyboard; text input stays active so hardware
            // keyboards and the in-game console still receive events.
            if ( SDL_TextInputActive( m_window ) )
                SDL_StopTextInput( m_window );
            SDL_StartTextInput( m_window );
        }
#endif
    }
public:
    int ReadTouches( MobileTouchEvent *events, int capacity ) override
    {
        AUTO_LOCK( m_eventMutex );
        int count=0;
        while (count<capacity && !m_touches.empty()) { events[count++]=m_touches.front(); m_touches.pop_front(); }
        return count;
    }
    void SetTouchMode( MobileTouchMode mode ) override
    {
        if (m_touchMode==mode) return;
        if (m_touchButton) PostTouchEvent(false,-1,-1);
        m_uiFingerActive=false;
        m_mouseFinger=false;
        m_touchMode=mode;
        UpdateGrab();
        AUTO_LOCK(m_eventMutex);
        m_touches.clear();
        m_touches.push_back({MOBILE_TOUCH_RESET,0,0,0});
    }
    MobileViewport GetTouchViewport() override
    {
        MobileViewport v={1,1,0,0,1,1};
        if (m_window)
        {
            SDL_GetWindowSize(m_window,&v.width,&v.height);
            SDL_Rect safe={0,0,v.width,v.height};
            if (v.width>0 && v.height>0 && SDL_GetWindowSafeArea(m_window,&safe))
            {
                v.left=float(safe.x)/v.width; v.top=float(safe.y)/v.height;
                v.right=float(safe.x+safe.w)/v.width; v.bottom=float(safe.y+safe.h)/v.height;
            }
        }
        return v;
    }
    bool ReadGyro( float &yaw, float &pitch ) override
    {
        yaw = pitch = 0;
#if !defined(ANDROID)
        return false;
#else
        const Uint64 now = SDL_GetTicksNS();
        float rateX = 0, rateY = 0;
        bool have = false;
        bool opened = false;
        {
            AUTO_LOCK(m_eventMutex);
            opened = m_gyro != nullptr;
            have = opened && m_gyroHave && m_hasFocus;
            rateX = m_gyroRateX;
            rateY = m_gyroRateY;
        }
        float dt = 0;
        if ( m_gyroRead && now > m_gyroRead )
            dt = float(now - m_gyroRead) * 1.0e-9f;
        m_gyroRead = now;
        if ( !have ) return opened;
        if ( !(dt > 0.f) || dt > 0.05f ) return true;
        float yawRate = 0, pitchRate = 0;
        MapDeviceGyroToView(CurrentGyroOrientation(), rateX, rateY, yawRate, pitchRate);
        yaw = yawRate * dt;
        pitch = pitchRate * dt;
        return true;
#endif
    }
    void *QueryInterface( const char *name ) override
    {
        if (!V_strcmp(name,MOBILE_INPUT_INTERFACE_VERSION)) return static_cast<IMobileInputSource *>(this);
        return !V_strcmp( name, SDLMGR_INTERFACE_VERSION ) ? this : nullptr;
    }
    InitReturnVal_t Init() override
    {
        if ( !SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) )
        {
            Warning( "SDL3 initialization failed: %s\n", SDL_GetError() );
            return INIT_FAILED;
        }
        m_videoInitialized = true;
        m_watching = SDL_AddEventWatch( Watch, this );
        if ( !m_watching )
        {
            Shutdown();
            return INIT_FAILED;
        }
#if !defined( ANDROID )
        // MaterialSystem queries the renderer before its first SetMode call.
        // Keep one window throughout initialization and subsequent mode changes.
        if ( !CommandLine()->FindParm("-textmode") )
        {
            Warning( "CSGO_TRACE: sdlmgr Init pre-window calling...\n" );
#ifdef __OHOS__
            // 复用 engine_startup 在 SDL_InitSubSystem 后建的窗口（OHOS 单窗口限制，
            // 且 DXVK 已把它注册为 swapchain 的 native window）
            m_window = SDL_GetWindowFromID( 1 );
#endif
            if ( !m_window )
                m_window = SDL_CreateWindow("", 640, 480, SDL_WINDOW_VULKAN |
                    SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
            Warning( "CSGO_TRACE: sdlmgr Init pre-window -> %p\n", m_window );
            if ( !m_window )
            {
                Warning("SDL3 Vulkan window failed: %s\n", SDL_GetError());
                Shutdown();
                return INIT_FAILED;
            }
            SetAssertDialogParent(m_window);
        }
#endif
        return INIT_OK;
    }
    void Shutdown() override
    {
        if ( m_watching ) SDL_RemoveEventWatch( Watch, this );
        m_watching = false;
        DestroyGameWindow();
#if defined(ANDROID)
        CloseGyro();
#endif
        if ( m_videoInitialized ) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        m_videoInitialized = false;
        AUTO_LOCK( m_eventMutex );
        m_events.clear();
        m_touches.clear();
    }
    bool CreateGameWindow( const char *title, bool windowed, int width, int height, bool desktopFriendly ) override
    {
        if ( width <= 0 || height <= 0 )
        {
            if ( m_window ) SDL_GetWindowSize(m_window, &width, &height);
            else
            {
                const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(PreferredDisplay());
                width = mode ? mode->w : 1024;
                height = mode ? mode->h : 768;
            }
        }
        if ( !m_window )
        {
            Warning( "CSGO_TRACE: SDL_CreateWindow(%dx%d VULKAN) calling...\n", width, height );
            m_window = SDL_CreateWindow( title ? title : "", width, height,
                SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | (windowed ? 0 : SDL_WINDOW_FULLSCREEN) );
            Warning( "CSGO_TRACE: SDL_CreateWindow -> %p\n", m_window );
        }
        if ( !m_window )
        {
            Warning( "SDL3 Vulkan window failed: %s\n", SDL_GetError() );
            return false;
        }
        Warning( "CSGO_TRACE: game window ready\n" );
        if ( title ) SDL_SetWindowTitle( m_window, title );
#if !defined( ANDROID )
        SetAssertDialogParent(m_window);
        if ( !SetWindowMode(!windowed, width, height, desktopFriendly) )
        {
            Warning("SDL3 window mode failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_ShowWindow(m_window);
        SDL_RaiseWindow(m_window);
        m_hasFocus = (SDL_GetWindowFlags(m_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#else
        SDL_SetHint( SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0" );
#endif
        // Text input stays active for hardware keyboards and the console; the
        // on-screen keyboard only appears on explicit text-entry focus.
        if ( !SDL_StartTextInput( m_window ) )
            Warning( "SDL3 text input initialization failed: %s\n", SDL_GetError() );
        UpdateGrab();
        return true;
    }
    bool WindowNormalized( float x, float y, float &nx, float &ny ) const
    {
        int width = 0, height = 0;
        if ( !m_window || !SDL_GetWindowSize(m_window, &width, &height) || width <= 0 || height <= 0 )
            return false;
        nx = x / float(width);
        ny = y / float(height);
        return true;
    }
    void PostTouchEvent( bool down, float x, float y )
    {
        int width = 0, height = 0;
        if ( !m_window || !SDL_GetWindowSize(m_window, &width, &height) || width <= 0 || height <= 0 )
            return;
        float scaleX, scaleY;
        WindowToRenderScale(scaleX, scaleY);
        const int px = int(SDL_lroundf(x * width * scaleX)), py = int(SDL_lroundf(y * height * scaleY));

        {
            // Resolve hover before both press and release. Cancellation moves
            // outside the window so it releases a button without clicking it.
            CCocoaEvent move = {};
            move.m_EventType = CocoaEvent_MouseMove;
            move.m_MousePos[0] = px;
            move.m_MousePos[1] = py;
            move.m_MouseButtonFlags = m_buttons;
            Post(move);
        }
        m_touchButton=down;
        m_buttons=m_hardwareButtons | (down ? COCOABUTTON_LEFT : 0);
        CCocoaEvent button = {};
        button.m_EventType = down ? CocoaEvent_MouseButtonDown : CocoaEvent_MouseButtonUp;
        button.m_MousePos[0] = px;
        button.m_MousePos[1] = py;
        button.m_MouseButton = COCOABUTTON_LEFT;
        button.m_MouseButtonFlags = m_buttons;
        button.m_nMouseClickCount = 1;
        Post(button);
    }
    void DestroyGameWindow() override
    {
#if !defined( ANDROID )
        SetAssertDialogParent(nullptr);
#endif
        if ( m_window )
        {
            SDL_StopTextInput( m_window );
            SDL_DestroyWindow( m_window );
        }
        m_androidScreenKeyboardShown = false;
        m_window = nullptr;
        m_width = m_height = 0;
        m_buttons = 0;
        m_hardwareButtons=0;
        m_uiFingerActive=m_touchButton=false;
        m_deltaX = m_deltaY = 0;
        m_wheelDelta = 0;
    }
    int GetEvents( CCocoaEvent *events, int maximum, bool debug ) override
    {
        if ( debug ) return 0;
        AUTO_LOCK( m_eventMutex );
        int count = 0;
        while ( count < maximum && !m_events.empty() )
        {
            events[count++] = m_events.front();
            m_events.pop_front();
        }
        return count;
    }
    void PumpWindowsMessageLoop() override
    {
#if defined(ANDROID)
        EnsureGyro();
#endif
        if(m_resetTouchPointer.exchange(false))
        {
            if(m_touchButton)PostTouchEvent(false,-1,-1);
            m_uiFingerActive=false;
        }
        UpdateAndroidScreenKeyboard();
        float scaleX = 1.0f, scaleY = 1.0f;
        if ( m_visible ) WindowToRenderScale(scaleX, scaleY);
        SDL_Event event;
        for ( int count = 0; count < 256 && SDL_PollEvent(&event); ++count )
        {
            CCocoaEvent translated = {};
            switch ( event.type )
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                translated.m_EventType = CocoaEvent_AppQuit;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                translated.m_EventType = event.key.down ? CocoaEvent_KeyDown : CocoaEvent_KeyUp;
                translated.m_VirtualKeyCode = VirtualKey(event.key);
                translated.m_ModifierKeyMask = Modifiers(event.key.mod);
                break;
            case SDL_EVENT_TEXT_INPUT:
            {
                // SDL always supplies UTF-8. The legacy wide-string helper
                // depends on LC_CTYPE and loses non-ASCII input in the C locale.
                for ( const char *text = event.text.text; text && *text; )
                {
                    uchar32 codepoint = 0;
                    bool error = false;
                    const int bytes = V_UTF8ToUChar32(text, codepoint, error);
                    if ( bytes <= 0 ) break;
                    text += bytes;
                    if ( error ) continue;
                    translated.m_EventType = CocoaEvent_KeyDown;
                    translated.m_UnicodeKey = translated.m_UnicodeKeyUnmodified = wchar_t(codepoint);
                    translated.m_ModifierKeyMask = Modifiers(SDL_GetModState());
                    Post(translated);
                    translated.m_EventType = CocoaEvent_KeyUp;
                    Post(translated);
                }
                continue;
            }
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.which==SDL_TOUCH_MOUSEID) continue;
#if !defined(ANDROID)
                if (m_touchMode==MOBILE_TOUCH_GAME && m_mouseFinger)
                {
                    float nx = 0, ny = 0;
                    if (WindowNormalized(event.motion.x, event.motion.y, nx, ny))
                        PostTouch({MOBILE_TOUCH_MOVE, kMouseFinger, nx, ny});
                    continue;
                }
#endif
                translated.m_EventType = CocoaEvent_MouseMove;
                translated.m_MousePos[0] = int(SDL_lroundf(event.motion.x * scaleX));
                translated.m_MousePos[1] = int(SDL_lroundf(event.motion.y * scaleY));
                translated.m_MouseButtonFlags = m_buttons;
                m_deltaX += event.motion.xrel;
                m_deltaY += event.motion.yrel;
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
            case SDL_EVENT_FINGER_MOTION:
            {
                MobileTouchType type=event.type==SDL_EVENT_FINGER_DOWN ? MOBILE_TOUCH_DOWN
                    : event.type==SDL_EVENT_FINGER_UP ? MOBILE_TOUCH_UP
                    : event.type==SDL_EVENT_FINGER_CANCELED ? MOBILE_TOUCH_CANCEL : MOBILE_TOUCH_MOVE;
                PostTouch({type,int64_t(event.tfinger.fingerID),event.tfinger.x,event.tfinger.y});
                if (m_touchMode!=MOBILE_TOUCH_UI) continue;
                if (type==MOBILE_TOUCH_DOWN && !m_uiFingerActive)
                {
                    m_uiFingerActive=true; m_uiFinger=int64_t(event.tfinger.fingerID);
                    m_uiX=event.tfinger.x; m_uiY=event.tfinger.y;
                    PostTouchEvent(true,m_uiX,m_uiY);
                    continue;
                }
                if (!m_uiFingerActive || m_uiFinger!=int64_t(event.tfinger.fingerID)) continue;
                m_uiX=event.tfinger.x; m_uiY=event.tfinger.y;
                if (type==MOBILE_TOUCH_UP || type==MOBILE_TOUCH_CANCEL)
                {
                    PostTouchEvent(false,type==MOBILE_TOUCH_CANCEL?-1:m_uiX,type==MOBILE_TOUCH_CANCEL?-1:m_uiY);
                    m_uiFingerActive=false; continue;
                }
                int width = 0, height = 0;
                if ( !m_window || !SDL_GetWindowSize(m_window, &width, &height) || width <= 0 || height <= 0 )
                    continue;
                float scaleX, scaleY;
                WindowToRenderScale(scaleX, scaleY);
                CCocoaEvent move = {};
                move.m_EventType = CocoaEvent_MouseMove;
                move.m_MousePos[0] = int(SDL_lroundf(event.tfinger.x * width * scaleX));
                move.m_MousePos[1] = int(SDL_lroundf(event.tfinger.y * height * scaleY));
                move.m_MouseButtonFlags = m_buttons;
                Post(move);
                continue;
            }
            case SDL_EVENT_TEXT_EDITING:
            case SDL_EVENT_TEXT_EDITING_CANDIDATES:
                continue;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                if (event.button.which==SDL_TOUCH_MOUSEID) continue;
#if !defined(ANDROID)
                if (m_touchMode==MOBILE_TOUCH_GAME && event.button.button==SDL_BUTTON_LEFT)
                {
                    float nx = 0, ny = 0;
                    if (WindowNormalized(event.button.x, event.button.y, nx, ny))
                    {
                        m_mouseFinger = event.button.down;
                        PostTouch({event.button.down ? MOBILE_TOUCH_DOWN : MOBILE_TOUCH_UP, kMouseFinger, nx, ny});
                    }
                    continue;
                }
#endif
                const int button = event.button.button == SDL_BUTTON_MIDDLE ? 3
                    : event.button.button == SDL_BUTTON_RIGHT ? 2 : MIN(int(event.button.button), 5);
                if ( button < 1 ) continue;
                translated.m_MouseButton = 1 << (button-1);
                if ( event.button.down ) m_hardwareButtons |= translated.m_MouseButton;
                else m_hardwareButtons &= ~translated.m_MouseButton;
                m_buttons=m_hardwareButtons | (m_touchButton ? COCOABUTTON_LEFT : 0);
                translated.m_EventType = event.button.down ? CocoaEvent_MouseButtonDown : CocoaEvent_MouseButtonUp;
                translated.m_MouseButtonFlags = m_buttons;
                translated.m_nMouseClickCount = event.button.clicks;
                translated.m_MousePos[0] = int(SDL_lroundf(event.button.x * scaleX));
                translated.m_MousePos[1] = int(SDL_lroundf(event.button.y * scaleY));
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
            {
                // Legacy Cocoa events carry the vertical wheel in both coordinates.
                m_wheelDelta += event.wheel.y * (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
                const int scroll = int(m_wheelDelta);
                if ( !scroll ) continue;
                m_wheelDelta -= scroll;
                translated.m_EventType = CocoaEvent_MouseScroll;
                translated.m_MousePos[0] = translated.m_MousePos[1] = scroll;
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                translated.m_EventType = CocoaEvent_AppActivate;
                translated.m_ModifierKeyMask = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                m_hasFocus = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
                UpdateGrab();
                m_buttons = 0;
                m_hardwareButtons=0;
                m_uiFingerActive=m_touchButton=m_mouseFinger=false;
                PostTouch({MOBILE_TOUCH_RESET,0,0,0});
                m_deltaX = m_deltaY = 0;
                m_wheelDelta = 0;
#if defined(ANDROID)
                {
                    AUTO_LOCK(m_eventMutex);
                    m_gyroHave = false;
                }
                m_gyroRead = 0;
#endif
                break;
#if defined(ANDROID)
            case SDL_EVENT_SENSOR_UPDATE:
                if ( m_gyro && event.sensor.which == SDL_GetSensorID(m_gyro) )
                {
                    const float x = event.sensor.data[0], y = event.sensor.data[1];
                    AUTO_LOCK(m_eventMutex);
                    if ( UsableGyroRate(x) && UsableGyroRate(y) )
                    {
                        m_gyroRateX = x;
                        m_gyroRateY = y;
                        m_gyroHave = true;
                    }
                }
                continue;
#endif
            default: continue;
            }
            Post(translated);
        }
    }
    void SetCursorPosition( int x, int y ) override
    {
        float scaleX, scaleY;
        WindowToRenderScale(scaleX, scaleY);
        if (m_window) SDL_WarpMouseInWindow(m_window, x / scaleX, y / scaleY);
    }
    void SetWindowFullScreen( bool fullscreen, int width, int height, bool desktopFriendly ) override
    {
        if ( !m_window ) return;
        if ( !SetWindowMode(fullscreen, width, height, desktopFriendly) )
            Warning("SDL3 fullscreen change failed: %s\n", SDL_GetError());
    }
    bool IsWindowFullScreen() override { return m_window && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN); }
    void MoveWindow( int x, int y ) override { if (m_window) SDL_SetWindowPosition(m_window,x,y); }
    void SizeWindow( int width, int height ) override { if (m_window) SDL_SetWindowSize(m_window,width,height); }
    void SetApplicationIcon( const char *path ) override
    {
#if !defined( ANDROID )
        if ( !m_window || !path ) return;
        SDL_Surface *icon = SDL_LoadBMP(path);
        if ( icon )
        {
            SDL_SetWindowIcon(m_window, icon);
            SDL_DestroySurface(icon);
        }
#endif
    }
    void GetMouseDelta( int &x, int &y, bool ignore ) override
    {
        float scaleX = 1.0f, scaleY = 1.0f;
        if ( m_visible ) WindowToRenderScale(scaleX, scaleY);
        x = ignore ? 0 : int(SDL_lroundf(m_deltaX * scaleX));
        y = ignore ? 0 : int(SDL_lroundf(m_deltaY * scaleY));
        if (ignore) m_deltaX = m_deltaY = 0;
        else { m_deltaX -= x / scaleX; m_deltaY -= y / scaleY; }
    }
    void GetNativeDisplayInfo( int index, uint &width, uint &height, uint &refresh ) override
    {
        int count = 0;
        SDL_DisplayID *displays = SDL_GetDisplays(&count);
        const SDL_DisplayID display = displays && index >= 0 && index < count ? displays[index]
            : (m_window ? SDL_GetDisplayForWindow(m_window) : SDL_GetPrimaryDisplay());
        const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(display);
        width = mode ? uint(mode->w) : 0; height = mode ? uint(mode->h) : 0;
        refresh = mode ? uint(mode->refresh_rate + 0.5f) : 0;
        SDL_free(displays);
    }
    void RenderedSize( uint &width, uint &height, bool set ) override
    {
        if (set) { m_width = width; m_height = height; }
        else { width = m_width; height = m_height; }
    }
    void DisplayedSize( uint &width, uint &height ) override
    {
        int w = 0, h = 0;
        if (m_window) SDL_GetWindowSizeInPixels(m_window, &w, &h);
        width = uint(w); height = uint(h);
    }
    void WaitUntilUserInput( int milliseconds ) override
    {
        if (SDL_WaitEventTimeout(nullptr, milliseconds)) PumpWindowsMessageLoop();
    }
    void *GetWindowRef() override { return m_window; }
    void SetMouseVisible( bool visible ) override { m_visible = visible; UpdateGrab(); }
    int GetActiveDisplayIndex() override
    {
        const SDL_DisplayID active = m_window ? SDL_GetDisplayForWindow(m_window) : SDL_GetPrimaryDisplay();
        int count = 0, result = 0;
        SDL_DisplayID *displays = SDL_GetDisplays(&count);
        for (int i = 0; displays && i < count; ++i) if (displays[i] == active) result = i;
        SDL_free(displays);
        return result;
    }
    void SetMouseCursor( SDL_Cursor *cursor ) override { if (cursor) SDL_SetCursor(cursor); }
    void SetForbidMouseGrab( bool forbid ) override { m_forbidGrab = forbid; UpdateGrab(); }
    void OnFrameRendered() override {}
    void SetGammaRamp( const uint16 *, const uint16 *, const uint16 * ) override {} // Gamma belongs to the D3D9 device.
    void GetDesiredPixelFormatAttribsAndRendererInfo( uint **attributes, uint *count, GLMRendererInfoFields * ) override
    { *attributes = nullptr; *count = 0; }
    PseudoGLContextPtr GetGLContextForWindow( void * ) override { return nullptr; }
    PseudoGLContextPtr GetMainContext() override { return nullptr; }
    PseudoGLContextPtr CreateExtraContext() override { return nullptr; }
    void DeleteContext( PseudoGLContextPtr ) override {}
    bool MakeContextCurrent( PseudoGLContextPtr ) override { return false; }
    GLMDisplayDB *GetDisplayDB() override { return nullptr; }
    void GetStackCrawl( CStackCrawlParams * ) override {}
    double GetPrevGLSwapWindowTime() override { return 0; }
    void ShowPixels( CShowPixelsParams * ) override { Error("ToGL presentation requested on the native DXVK backend\n"); }
};
}

ILauncherMgr *g_pLauncherMgr = nullptr;
void *CreateSDLMgr()
{
    static CSDL3Mgr manager;
    g_pLauncherMgr = &manager;
    return g_pLauncherMgr;
}
