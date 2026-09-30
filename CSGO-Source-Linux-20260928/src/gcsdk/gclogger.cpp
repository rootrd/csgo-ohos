//====== Copyright (c), Valve Corporation, All rights reserved. =======
//
// Purpose: GC emit groups routed to the engine console/log.
//
//=============================================================================

#include "gcsdk/gcclientsdk.h"
#include "tier0/memdbgon.h"

namespace GCSDK
{

static void EmitFormatted( const CGCEmitGroup &group, CGCEmitGroup::EMsgLevel eLevel, const char *pchMsg, va_list vaArgs )
{
	char szMessage[4096];
	V_vsnprintf( szMessage, sizeof( szMessage ), pchMsg, vaArgs );
	switch ( eLevel )
	{
	case CGCEmitGroup::kMsg_Error:
	case CGCEmitGroup::kMsg_Warning:
		Warning( "[%s] %s", group.GetName(), szMessage );
		break;
	case CGCEmitGroup::kMsg_Msg:
		Msg( "[%s] %s", group.GetName(), szMessage );
		break;
	default:
		DevMsg( "[%s] %s", group.GetName(), szMessage );
		break;
	}
}

#define GC_EMIT_VARARGS( level ) \
	va_list vaArgs; va_start( vaArgs, pchMsg ); EmitFormatted( *this, level, pchMsg, vaArgs ); va_end( vaArgs )

void CGCEmitGroup::Internal_AssertError( const char *pchMsg, ... ) const
{
	GC_EMIT_VARARGS( kMsg_Error );
	AssertMsg1( false, "GC emit group %s reported an assert-level error", m_pszGroupName );
}
void CGCEmitGroup::Internal_Error( const char *pchMsg, ... ) const	{ GC_EMIT_VARARGS( kMsg_Error ); }
void CGCEmitGroup::Internal_Warning( const char *pchMsg, ... ) const	{ GC_EMIT_VARARGS( kMsg_Warning ); }
void CGCEmitGroup::Internal_Msg( const char *pchMsg, ... ) const		{ GC_EMIT_VARARGS( kMsg_Msg ); }
void CGCEmitGroup::Internal_Verbose( const char *pchMsg, ... ) const	{ GC_EMIT_VARARGS( kMsg_Verbose ); }
void CGCEmitGroup::Internal_Emit( EMsgLevel eLvl, const char *pchMsg, ... ) const { GC_EMIT_VARARGS( eLvl ); }

void CGCEmitGroup::AssertErrorV( const char *pchMsg, va_list vaArgs ) const
{
	EmitFormatted( *this, kMsg_Error, pchMsg, vaArgs );
	AssertMsg1( false, "GC emit group %s reported an assert-level error", m_pszGroupName );
}
void CGCEmitGroup::ErrorV( const char *pchMsg, va_list vaArgs ) const	{ EmitFormatted( *this, kMsg_Error, pchMsg, vaArgs ); }
void CGCEmitGroup::WarningV( const char *pchMsg, va_list vaArgs ) const	{ EmitFormatted( *this, kMsg_Warning, pchMsg, vaArgs ); }
void CGCEmitGroup::MsgV( const char *pchMsg, va_list vaArgs ) const		{ EmitFormatted( *this, kMsg_Msg, pchMsg, vaArgs ); }
void CGCEmitGroup::VerboseV( const char *pchMsg, va_list vaArgs ) const	{ EmitFormatted( *this, kMsg_Verbose, pchMsg, vaArgs ); }
void CGCEmitGroup::EmitV( EMsgLevel eLvl, const char *pchMsg, va_list vaArgs ) const { EmitFormatted( *this, eLvl, pchMsg, vaArgs ); }

#undef GC_EMIT_VARARGS

// Legacy groups referenced by the game code.
#define GC_DEFINE_EMIT_GROUP( VarName, GroupName ) \
	CGCEmitGroup VarName( #GroupName, "console_level_" #GroupName, "log_level_" #GroupName, "3", "3" )

GC_DEFINE_EMIT_GROUP( SPEW_SYSTEM_MISC, SystemMisc );
GC_DEFINE_EMIT_GROUP( SPEW_JOB, Job );
GC_DEFINE_EMIT_GROUP( SPEW_CONSOLE, Console );
GC_DEFINE_EMIT_GROUP( SPEW_GC, GC );
GC_DEFINE_EMIT_GROUP( SPEW_SQL, SQL );
GC_DEFINE_EMIT_GROUP( SPEW_NETWORK, Network );
GC_DEFINE_EMIT_GROUP( SPEW_SHAREDOBJ, SharedObj );
GC_DEFINE_EMIT_GROUP( SPEW_MICROTXN, MicroTxn );
GC_DEFINE_EMIT_GROUP( SPEW_PROMO, Promo );
GC_DEFINE_EMIT_GROUP( SPEW_PKGITEM, PkgItem );
GC_DEFINE_EMIT_GROUP( SPEW_ECONOMY, Economy );
GC_DEFINE_EMIT_GROUP( SPEW_THREADS, Threads );

#undef GC_DEFINE_EMIT_GROUP

static CGCEmitGroup::EMsgLevel ClampLevel( int iLevel )
{
	return (CGCEmitGroup::EMsgLevel)clamp( iLevel, (int)CGCEmitGroup::kMsg_Error, (int)CGCEmitGroup::kMsg_Verbose );
}

void EGInternal_EmitInfo( const CGCEmitGroup& Group, int iLevel, int iLevelLog, const char *pchMsg, ... )
{
	va_list vaArgs;
	va_start( vaArgs, pchMsg );
	EmitInfoV( Group, iLevel, iLevelLog, pchMsg, vaArgs );
	va_end( vaArgs );
}

void EmitInfoV( const CGCEmitGroup& Group, int iLevel, int iLevelLog, const char *pchMsg, va_list vaArgs )
{
	EmitFormatted( Group, ClampLevel( MIN( iLevel, iLevelLog ) ), pchMsg, vaArgs );
}

void EmitWarning( const CGCEmitGroup& Group, int iLevel, const char *pchMsg, ... )
{
	(void)iLevel;
	va_list vaArgs;
	va_start( vaArgs, pchMsg );
	EmitFormatted( Group, CGCEmitGroup::kMsg_Warning, pchMsg, vaArgs );
	va_end( vaArgs );
}

void EmitError( const CGCEmitGroup& Group, const char *pchMsg, ... )
{
	va_list vaArgs;
	va_start( vaArgs, pchMsg );
	EmitFormatted( Group, CGCEmitGroup::kMsg_Error, pchMsg, vaArgs );
	va_end( vaArgs );
}

void EmitAssertError( const CGCEmitGroup& Group, const char *pchMsg, ... )
{
	va_list vaArgs;
	va_start( vaArgs, pchMsg );
	Group.AssertErrorV( pchMsg, vaArgs );
	va_end( vaArgs );
}

} // namespace GCSDK
