//====== Copyright (c), Valve Corporation, All rights reserved. =======
//
// Purpose: Reference-counted received network packet buffer.
//
//=============================================================================

#include "gcsdk/gcclientsdk.h"
#include "tier0/memdbgon.h"

namespace GCSDK
{

CNetPacket::CNetPacket()
	: m_cRef( 1 )
	, m_cubData( 0 )
	, m_pubData( NULL )
{
}

CNetPacket::~CNetPacket()
{
	free( m_pubData );
}

void CNetPacket::Init( uint32 cubData, const void* pCopyData )
{
	free( m_pubData );
	m_pubData = static_cast<uint8 *>( malloc( cubData ) );
	m_cubData = m_pubData ? cubData : 0;
	if ( m_pubData && pCopyData )
		memcpy( m_pubData, pCopyData, cubData );
}

void CNetPacket::InitAdoptBuffer( uint32 cubData, uint8* pubData )
{
	free( m_pubData );
	m_pubData = pubData;
	m_cubData = cubData;
}

void CNetPacket::OrphanBuffer()
{
	m_pubData = NULL;
	m_cubData = 0;
}

void CNetPacket::AddRef()
{
	ThreadInterlockedIncrement( &m_cRef );
}

void CNetPacket::Release()
{
	if ( ThreadInterlockedDecrement( &m_cRef ) == 0 )
		delete this;
}

} // namespace GCSDK
