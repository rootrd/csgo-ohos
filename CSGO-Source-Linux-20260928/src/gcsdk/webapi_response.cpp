//====== Copyright (c), Valve Corporation, All rights reserved. =======
//
// Purpose: JSON string emission shared by the Web API response writers.
//
//=============================================================================

#include "gcsdk/gcclientsdk.h"
#include "tier0/memdbgon.h"

// Writes pchValue as a quoted JSON string. UTF-8 passes through unchanged;
// quotes, backslashes and control characters are escaped.
void EmitJSONString( CUtlBuffer &outputBuffer, const char *pchValue )
{
	outputBuffer.PutChar( '"' );
	for ( const unsigned char *p = (const unsigned char *)( pchValue ? pchValue : "" ); *p; ++p )
	{
		switch ( *p )
		{
		case '"':  outputBuffer.Put( "\\\"", 2 ); break;
		case '\\': outputBuffer.Put( "\\\\", 2 ); break;
		case '\b': outputBuffer.Put( "\\b", 2 ); break;
		case '\f': outputBuffer.Put( "\\f", 2 ); break;
		case '\n': outputBuffer.Put( "\\n", 2 ); break;
		case '\r': outputBuffer.Put( "\\r", 2 ); break;
		case '\t': outputBuffer.Put( "\\t", 2 ); break;
		default:
			if ( *p < 0x20 )
			{
				char escape[7];
				V_snprintf( escape, sizeof( escape ), "\\u%04x", *p );
				outputBuffer.Put( escape, 6 );
			}
			else
				outputBuffer.PutChar( (char)*p );
			break;
		}
	}
	outputBuffer.PutChar( '"' );
}
