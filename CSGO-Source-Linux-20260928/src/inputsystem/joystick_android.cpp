// SDL3 gamepads are sampled on the engine thread. Device IDs are stable SDL
// instance IDs, not SDL2's indices, and hotplug must also work with no pad open.
#include <SDL3/SDL.h>
#include "inputsystem.h"
#include "tier0/icommandline.h"
#include "tier1/convar.h"

ConVar joy_axisbutton_threshold("joy_axisbutton_threshold", "0.3", FCVAR_ARCHIVE);
ConVar joy_axis_deadzone("joy_axis_deadzone", "0.2", FCVAR_ARCHIVE);
ConVar joy_active("joy_active", "-1", FCVAR_NONE, "Active gamepad instance ID; -1 selects the first available.");
ConVar joy_gamecontroller_config("joy_gamecontroller_config", "", FCVAR_ARCHIVE, "SDL gamepad mappings.");

static ButtonCode_t GamepadButton(int button)
{
    // Source's XInput names match SDL3's standard face-button order only for the
    // first four entries; emit the controller-layout button, not the raw number.
    switch (button)
    {
    case SDL_GAMEPAD_BUTTON_SOUTH: return KEY_XBUTTON_A;
    case SDL_GAMEPAD_BUTTON_EAST: return KEY_XBUTTON_B;
    case SDL_GAMEPAD_BUTTON_WEST: return KEY_XBUTTON_X;
    case SDL_GAMEPAD_BUTTON_NORTH: return KEY_XBUTTON_Y;
    case SDL_GAMEPAD_BUTTON_BACK: case SDL_GAMEPAD_BUTTON_GUIDE: return KEY_XBUTTON_BACK;
    case SDL_GAMEPAD_BUTTON_START: return KEY_XBUTTON_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return KEY_XBUTTON_STICK1;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return KEY_XBUTTON_STICK2;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return KEY_XBUTTON_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return KEY_XBUTTON_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return KEY_XBUTTON_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return KEY_XBUTTON_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return KEY_XBUTTON_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return KEY_XBUTTON_RIGHT;
    default: return BUTTON_CODE_NONE;
    }
}

void CInputSystem::InitializeJoysticks()
{
    V_memset(m_pJoystickInfo, 0, sizeof(m_pJoystickInfo));
    for (int i = 0; i < MAX_JOYSTICKS; ++i) m_pJoystickInfo[i].m_nDeviceId = -1;
    m_nJoystickCount = 0;
    if (CommandLine()->FindParm("-nojoy")) return;
    // SDL3 polls the gamepad event queue itself; the game reads the current
    // state each frame, mirroring joystick_linux.cpp's SDL event watcher model.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        Warning("SDL3 gamepad initialization failed: %s\n", SDL_GetError());
    SDL_PumpEvents();
}

void CInputSystem::ShutdownJoysticks()
{
    JoystickHotplugRemoved(m_pJoystickInfo[0].m_nDeviceId);
    if (SDL_WasInit(SDL_INIT_GAMEPAD)) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void CInputSystem::JoystickHotplugAdded(int id)
{
    auto &info = m_pJoystickInfo[0];
    if (info.m_pDevice || !SDL_IsGamepad(id)) return;
    if (joy_active.GetInt() >= 0 && joy_active.GetInt() != id) return;
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) return;
    info.m_pDevice = pad;
    info.m_nDeviceId = id;
    info.m_nButtonCount = SDL_GAMEPAD_BUTTON_COUNT;
    // Seed Source's edge detector from the live state. Emitting an artificial
    // press for a button held while the pad opens can launch the pause menu or
    // console during startup; the user must release and press it deliberately.
    uint32 initialButtons = 0;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT && button < 32; ++button)
        if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(button)))
            initialButtons |= 1u << button;
    info.m_nLastPolledButtons = initialButtons;
    m_androidTriggerLeft = 0;
    m_androidTriggerRight = 0;
    InputState_t &state = m_InputState[m_bIsPolling];
    for (int destination = 0; destination < MAX_JOYSTICK_AXES; ++destination)
    {
        const AnalogCode_t code = JOYSTICK_AXIS(0, (JoystickAxis_t)destination);
        state.m_pAnalogValue[code] = 0;
        state.m_pAnalogDelta[code] = 0;
    }
    V_memset(m_appXKeys[0], 0, sizeof(m_appXKeys[0]));
    m_nJoystickCount = 1;
    m_bXController = true;
    EnableJoystickInput(0, true);
    ConVarRef found("joy_xcontroller_found"), joystick("joystick");
    if (found.IsValid()) found.SetValue(1);
    if (joystick.IsValid()) joystick.SetValue(1);
    Msg("SDL3 gamepad connected: %s (ID %d)\n", SDL_GetGamepadName(pad), id);
}

void CInputSystem::JoystickHotplugRemoved(int id)
{
    auto &info = m_pJoystickInfo[0];
    if (!info.m_pDevice || info.m_nDeviceId != id) return;
    // Release held controls before closing the device to avoid stuck input.
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT && button < 32; ++button)
        if (info.m_nLastPolledButtons & (1u << button)) JoystickButtonRelease(id, button);
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) JoystickAxisMotion(id, axis, 0);
    SDL_CloseGamepad(static_cast<SDL_Gamepad *>(info.m_pDevice));
    info.m_pDevice = nullptr;
    info.m_nDeviceId = -1;
    info.m_nButtonCount = 0;
    info.m_nLastPolledButtons = 0;
    m_nJoystickCount = 0;
    m_bXController = false;
    EnableJoystickInput(0, false);
    ConVarRef found("joy_xcontroller_found");
    if (found.IsValid()) found.SetValue(0);
}

void CInputSystem::PollJoystick()
{
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) return;
    SDL_UpdateGamepads();
    auto &info = m_pJoystickInfo[0];
    SDL_Gamepad *pad = static_cast<SDL_Gamepad *>(info.m_pDevice);
    if (pad && (!SDL_GamepadConnected(pad) || (joy_active.GetInt() >= 0 && joy_active.GetInt() != info.m_nDeviceId)))
        JoystickHotplugRemoved(info.m_nDeviceId);
    if (!info.m_pDevice)
    {
        int count = 0;
        SDL_JoystickID *devices = SDL_GetGamepads(&count);
        for (int i = 0; i < count && !info.m_pDevice; ++i) JoystickHotplugAdded(devices[i]);
        SDL_free(devices);
    }
    pad = static_cast<SDL_Gamepad *>(info.m_pDevice);
    if (!pad) return;
    uint32 buttons = 0;
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT && button < 32; ++button)
    {
        if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(button))) buttons |= 1u << button;
        if ((buttons ^ info.m_nLastPolledButtons) & (1u << button))
        {
            if (buttons & (1u << button)) JoystickButtonPress(info.m_nDeviceId, button);
            else JoystickButtonRelease(info.m_nDeviceId, button);
        }
    }
    info.m_nLastPolledButtons = buttons;
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
        JoystickAxisMotion(info.m_nDeviceId, axis, SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(axis)));
}

void CInputSystem::JoystickButtonPress(int id, int button)
{
    ButtonCode_t code = GamepadButton(button);
    if (id == m_pJoystickInfo[0].m_nDeviceId && code != BUTTON_CODE_NONE)
        PostButtonPressedEvent(IE_ButtonPressed, m_nLastSampleTick, code, code);
}
void CInputSystem::JoystickButtonRelease(int id, int button)
{
    ButtonCode_t code = GamepadButton(button);
    if (id == m_pJoystickInfo[0].m_nDeviceId && code != BUTTON_CODE_NONE)
        PostButtonReleasedEvent(IE_ButtonReleased, m_nLastSampleTick, code, code);
}
void CInputSystem::AxisAnalogButtonEvent(ButtonCode_t code, bool down, int tick)
{
    int index = code - JOYSTICK_FIRST_AXIS_BUTTON;
    if ((m_appXKeys[0][index].repeats > 0) == down) return;
    if (down) PostButtonPressedEvent(IE_ButtonPressed, tick, code, code);
    else PostButtonReleasedEvent(IE_ButtonReleased, tick, code, code);
    m_appXKeys[0][index].repeats = down ? 1 : 0;
}
void CInputSystem::JoystickAxisMotion(int id, int axis, int value)
{
    if (id != m_pJoystickInfo[0].m_nDeviceId) return;
    if (abs(value) < joy_axis_deadzone.GetFloat()*32767.0f) value = 0;
    int threshold = joy_axisbutton_threshold.GetFloat()*32767.0f;
    JoystickAxis_t destination;
    switch (axis)
    {
    case SDL_GAMEPAD_AXIS_LEFTX:
        destination = JOY_AXIS_X;
        AxisAnalogButtonEvent(KEY_XSTICK1_LEFT, value < -threshold, m_nLastSampleTick);
        AxisAnalogButtonEvent(KEY_XSTICK1_RIGHT, value > threshold, m_nLastSampleTick);
        break;
    case SDL_GAMEPAD_AXIS_LEFTY:
        destination = JOY_AXIS_Y;
        AxisAnalogButtonEvent(KEY_XSTICK1_UP, value < -threshold, m_nLastSampleTick);
        AxisAnalogButtonEvent(KEY_XSTICK1_DOWN, value > threshold, m_nLastSampleTick);
        break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
        destination = JOY_AXIS_U;
        AxisAnalogButtonEvent(KEY_XSTICK2_LEFT, value < -threshold, m_nLastSampleTick);
        AxisAnalogButtonEvent(KEY_XSTICK2_RIGHT, value > threshold, m_nLastSampleTick);
        break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
        destination = JOY_AXIS_R;
        AxisAnalogButtonEvent(KEY_XSTICK2_UP, value < -threshold, m_nLastSampleTick);
        AxisAnalogButtonEvent(KEY_XSTICK2_DOWN, value > threshold, m_nLastSampleTick);
        break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
        destination = JOY_AXIS_Z;
        AxisAnalogButtonEvent(KEY_XBUTTON_LTRIGGER, value > threshold, m_nLastSampleTick);
        m_androidTriggerLeft = value;
        value = m_androidTriggerLeft - m_androidTriggerRight;
        break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        destination = JOY_AXIS_Z;
        AxisAnalogButtonEvent(KEY_XBUTTON_RTRIGGER, value > threshold, m_nLastSampleTick);
        m_androidTriggerRight = value;
        value = m_androidTriggerLeft - m_androidTriggerRight;
        break;
    default: return;
    }
    const AnalogCode_t code = JOYSTICK_AXIS(0, destination);
    InputState_t &state = m_InputState[m_bIsPolling];
    state.m_pAnalogDelta[code] = value - state.m_pAnalogValue[code];
    state.m_pAnalogValue[code] = value;
    if (state.m_pAnalogDelta[code])
        PostEvent(IE_AnalogValueChanged, m_nLastSampleTick, code, value, state.m_pAnalogDelta[code]);
}
void CInputSystem::SetXDeviceRumble(float left, float right, int)
{
    auto *pad = static_cast<SDL_Gamepad *>(m_pJoystickInfo[0].m_pDevice);
    if (pad) SDL_RumbleGamepad(pad, Uint16(clamp(left,0.0f,1.0f)*65535), Uint16(clamp(right,0.0f,1.0f)*65535), 0xffffffffu);
}
// PollJoystick handles buttons, axes and the gamepad D-pad together.
void CInputSystem::JoystickButtonEvent(ButtonCode_t button, int sample)
{ PostButtonPressedEvent(IE_ButtonPressed, sample, button, button); }
void CInputSystem::UpdateJoystickButtonState(int) { PollJoystick(); }
void CInputSystem::UpdateJoystickPOVControl(int) {}
