//========= Copyright, Valve Corporation, All rights reserved. ============//
//
// SDL2 GameController/Joystick/Haptic -> SDL3 gamepad API compatibility shim
// for SDL3 builds. Panorama's controller layer (controller.h/.cpp) was
// written against the SDL2 GameController API. SDL3 ships SDL_oldnames.h, which
// maps the pure renames when SDL_ENABLE_OLD_NAMES is set, so we lean on that
// instead of hand-maintaining aliases. Only the handful of calls SDL dropped
// from old_names (device-GUID-by-id, GUID stringize) are provided here; the
// genuine semantic changes (device enumeration, power query, event watcher
// signature) are handled with USE_SDL3 branches in controller.cpp.
//
#ifndef PANORAMA_SDL3_GAMEPAD_COMPAT_H
#define PANORAMA_SDL3_GAMEPAD_COMPAT_H
#pragma once

#ifndef SDL_ENABLE_OLD_NAMES
#define SDL_ENABLE_OLD_NAMES
#endif
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_guid.h>
#include <SDL3/SDL_oldnames.h>

// device identity is an instance id in SDL3; the SDL2 "device GUID" query that
// took a device index has no old_names mapping.
#define SDL_JoystickGetDeviceGUID SDL_GetJoystickGUIDForID

// SDL2's SDL_JoystickGetGUIDString(guid, buf, len) -> SDL3 SDL_GUIDToString.
static inline void SDL_JoystickGetGUIDString( SDL_GUID guid, char *pszGUID, int cbGUID )
{
	SDL_GUIDToString( guid, pszGUID, cbGUID );
}

#endif // PANORAMA_SDL3_GAMEPAD_COMPAT_H
