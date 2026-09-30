// Android has no desktop Steam client. Keep the SDK's unavailable-service
// behavior (invalid handles, null interfaces, failed initialization) so the
// engine's NO_STEAM path can use its own loopback/UDP transport.
#if !defined(ANDROID) || !defined(NO_STEAM)
#error This backend is only for the Android offline build.
#endif

#include "tier0/dbg.h"
#include "tier1/strtools.h"
#include "steam/steam_api.h"
#include "steam/steam_gameserver.h"
#include "steamnetworkingsockets/isteamnetworkingsockets.h"
#include "steamnetworkingsockets/isteamnetworkingutils.h"

bool S_CALLTYPE SteamAPI_Init() { return false; }
extern "C" bool S_CALLTYPE SteamAPI_InitSafe() { return false; }
bool S_CALLTYPE SteamInternal_Init() { return false; }
bool S_CALLTYPE SteamAPI_IsSteamRunning() { return false; }
bool S_CALLTYPE SteamAPI_RestartAppIfNecessary( uint32 ) { return false; }
void S_CALLTYPE SteamAPI_Shutdown() {}
void S_CALLTYPE SteamAPI_RunCallbacks() {}
void S_CALLTYPE SteamAPI_ReleaseCurrentThreadMemory() {}
void S_CALLTYPE SteamAPI_RegisterCallback( CCallbackBase *, int ) {}
void S_CALLTYPE SteamAPI_UnregisterCallback( CCallbackBase * ) {}
void S_CALLTYPE SteamAPI_RegisterCallResult( CCallbackBase *, SteamAPICall_t ) {}
void S_CALLTYPE SteamAPI_UnregisterCallResult( CCallbackBase *, SteamAPICall_t ) {}
void SteamAPI_SetTryCatchCallbacks( bool ) {}
HSteamUser SteamAPI_GetHSteamUser() { return 0; }
HSteamPipe SteamAPI_GetHSteamPipe() { return 0; }
HSteamUser Steam_GetHSteamUserCurrent() { return 0; }
HSteamUser GetHSteamUser() { return 0; }
HSteamPipe GetHSteamPipe() { return 0; }
const char *SteamAPI_GetSteamInstallPath() { return nullptr; }
void Steam_RunCallbacks( HSteamPipe, bool ) {}
void Steam_RegisterInterfaceFuncs( void * ) {}
void *S_CALLTYPE SteamInternal_CreateInterface( const char * ) { return nullptr; }
void *S_CALLTYPE SteamGameServerInternal_CreateInterface( const char * ) { return nullptr; }
void S_CALLTYPE SteamAPI_WriteMiniDump( uint32, void *, uint32 ) {}
void S_CALLTYPE SteamAPI_SetMiniDumpComment( const char * ) {}

bool S_CALLTYPE SteamInternal_GameServer_Init( uint32, uint16, uint16, uint16, EServerMode, const char * ) { return false; }
void SteamGameServer_Shutdown() {}
void SteamGameServer_RunCallbacks() {}
bool SteamGameServer_BSecure() { return false; }
uint64 SteamGameServer_GetSteamID() { return 0; }
HSteamPipe S_CALLTYPE SteamGameServer_GetHSteamPipe() { return 0; }
HSteamUser S_CALLTYPE SteamGameServer_GetHSteamUser() { return 0; }
CSteamGameServerAPIContext *S_CALLTYPE SteamInternal_GlobalContextGameServerPtr( uint32 size )
{
    static CSteamGameServerAPIContext context;
    return size == sizeof(context) ? &context : nullptr;
}

// Relay calls must be rejected at the engine boundary. Make any missed call
// report the unavailable service instead of returning a fabricated interface.
ISteamNetworkingSockets *SteamNetworkingSockets()
{
    Error("Steam relay transport is unavailable in the Android offline build.\n");
    return nullptr;
}
ISteamNetworkingSockets *SteamNetworkingSocketsGameServer()
{
    Error("Steam game-server transport is unavailable in the Android offline build.\n");
    return nullptr;
}
ISteamNetworkingUtils *SteamNetworkingUtils() { return nullptr; }
void SteamDatagramClient_SetPartner( const char *, int ) {}
void SteamDatagramClient_Internal_SteamAPIKludge( FSteamAPI_RegisterCallback, FSteamAPI_UnregisterCallback,
                                                FSteamAPI_RegisterCallResult, FSteamAPI_UnregisterCallResult ) {}
bool SteamDatagramClient_Init_InternalV6( SteamDatagramErrMsg &error, FSteamInternal_CreateInterface, HSteamUser, HSteamPipe )
{
    V_strncpy(error, "Steam relay transport is unavailable in the Android offline build", sizeof(error));
    return false;
}
bool SteamDatagramServer_Init_Internal( SteamDatagramErrMsg &error, FSteamInternal_CreateInterface, HSteamUser, HSteamPipe )
{
    V_strncpy(error, "Steam game-server transport is unavailable in the Android offline build", sizeof(error));
    return false;
}
void SteamDatagramClient_Kill() {}
void SteamDatagramServer_Kill() {}
void SteamNetworkingSockets_SetDebugOutputFunction( int, FSteamNetworkingSocketsDebugOutput ) {}
