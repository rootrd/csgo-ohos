//====== Copyright (c), Valve Corporation, All rights reserved. =======
//
// Purpose: Protobuf GC message wrappers, per-type message pools and the
//			protobuf net packet view used to receive them.
//
//=============================================================================

#include "gcsdk/gcclientsdk.h"
#include <limits.h>
#include "tier0/memdbgon.h"

namespace GCSDK
{

using ::google::protobuf::Message;

CThreadMutex CProtoBufMsgBase::s_PoolRegMutex;

//-----------------------------------------------------------------------------
// CProtoBufMsgMemoryPoolBase
//-----------------------------------------------------------------------------
CProtoBufMsgMemoryPoolBase::CProtoBufMsgMemoryPoolBase( uint32 unTargetLow, uint32 unTargetHigh )
	: m_unAllocHitCounter( 0 )
	, m_unAllocMissCounter( 0 )
	, m_unAllocated( 0 )
	, m_unTargetCountLow( unTargetLow )
	, m_unTargetCountHigh( unTargetHigh )
{
	m_pTSQueueFreeObjects = new CTSQueue< Message * >;
}

CProtoBufMsgMemoryPoolBase::~CProtoBufMsgMemoryPoolBase()
{
	// The derived destructor drains the queue through PopItem before this runs.
	Assert( m_pTSQueueFreeObjects->Count() == 0 );
	delete m_pTSQueueFreeObjects;
}

Message *CProtoBufMsgMemoryPoolBase::Alloc()
{
	Message *pMsg = NULL;
	if ( m_pTSQueueFreeObjects->PopItem( &pMsg ) )
	{
		++m_unAllocHitCounter;
		return pMsg;
	}
	++m_unAllocMissCounter;
	pMsg = InternalAlloc();
	if ( pMsg )
		++m_unAllocated;
	return pMsg;
}

void CProtoBufMsgMemoryPoolBase::Free( Message *pMsg )
{
	if ( !pMsg )
		return;
	// Pooled messages are handed out empty; clear on the way in.
	pMsg->Clear();
	if ( (uint32)m_pTSQueueFreeObjects->Count() >= m_unTargetCountHigh )
	{
		InternalFree( pMsg );
		--m_unAllocated;
		return;
	}
	m_pTSQueueFreeObjects->PushItem( pMsg );
}

uint32 CProtoBufMsgMemoryPoolBase::GetEstimatedSize()
{
	// The queue cannot be walked safely; report the live object count times the
	// base message size as a lower bound for the pool dump.
	return (uint32)m_unAllocated * (uint32)sizeof( Message );
}

bool CProtoBufMsgMemoryPoolBase::PopItem( Message **ppMsg )
{
	return m_pTSQueueFreeObjects->PopItem( ppMsg );
}

//-----------------------------------------------------------------------------
// CProtoBufMsgMemoryPoolMgr
//-----------------------------------------------------------------------------
CProtoBufMsgMemoryPoolMgr *GProtoBufMsgMemoryPoolMgr()
{
	// Constructed on first use so the steammessages descriptors are already registered.
	static CProtoBufMsgMemoryPoolMgr s_PoolMgr;
	return &s_PoolMgr;
}

CProtoBufMsgMemoryPoolMgr::CProtoBufMsgMemoryPoolMgr()
{
	m_vecMsgPools.AddToTail( &m_PoolHeaders );
}

CProtoBufMsgMemoryPoolMgr::~CProtoBufMsgMemoryPoolMgr()
{
	// The template statics are raw pointers; the manager owns the pools they
	// allocate. The header pool is a member and is destroyed automatically.
	for ( int i = 0; i < m_vecMsgPools.Count(); ++i )
		if ( m_vecMsgPools[i] != &m_PoolHeaders )
			delete m_vecMsgPools[i];
}

void CProtoBufMsgMemoryPoolMgr::RegisterPool( CProtoBufMsgMemoryPoolBase *pPool )
{
	// Callers hold CProtoBufMsgBase::s_PoolRegMutex.
	if ( pPool && m_vecMsgPools.Find( pPool ) == m_vecMsgPools.InvalidIndex() )
		m_vecMsgPools.AddToTail( pPool );
}

void CProtoBufMsgMemoryPoolMgr::DumpPoolInfo()
{
	Msg( "Protobuf message pools: %d\n", m_vecMsgPools.Count() );
	for ( int i = 0; i < m_vecMsgPools.Count(); ++i )
	{
		CProtoBufMsgMemoryPoolBase *pPool = m_vecMsgPools[i];
		Msg( "  %-48s allocated %5u free %5u hits %8u misses %6u ~%u bytes\n", pPool->GetName().Get(),
			pPool->GetAllocated(), pPool->GetFree(), pPool->GetAllocHitCount(), pPool->GetAllocMissCount(),
			pPool->GetEstimatedSize() );
	}
}

//-----------------------------------------------------------------------------
// CProtoBufNetPacket
//-----------------------------------------------------------------------------
CProtoBufNetPacket::CProtoBufNetPacket( CNetPacket *pNetPacket, GCProtoBufMsgSrc eReplyType, const CSteamID steamID,
	uint32 unClientAddr, uint32 nGCDirIndex, MsgType_t msgType )
	: m_pNetPacket( pNetPacket )
	, m_pHeader( NULL )
	, m_steamID( steamID )
	, m_msgType( msgType )
	, m_bIsValid( false )
{
	// Routing metadata (source type, client address, GC index) is only written
	// by the GC itself; a client view of a packet keeps the header it received.
	(void)eReplyType; (void)unClientAddr; (void)nGCDirIndex;
	m_pNetPacket->AddRef();
	m_pHeader = GProtoBufMsgMemoryPoolMgr()->AllocProtoBufHdr();
	const uint32 cubData = CubData();
	if ( cubData >= sizeof( ProtoBufMsgHeader_t ) )
	{
		const ProtoBufMsgHeader_t &fixed = GetFixedHeader();
		if ( ( fixed.m_EMsgFlagged & k_EMsgProtoBufFlag ) &&
			 sizeof( ProtoBufMsgHeader_t ) + (uint64)fixed.m_cubProtoBufExtHdr <= cubData )
		{
			m_bIsValid = m_pHeader->ParseFromArray( PubData() + sizeof( ProtoBufMsgHeader_t ), fixed.m_cubProtoBufExtHdr );
		}
	}
	if ( m_bIsValid && !m_pHeader->has_client_steam_id() && steamID.IsValid() )
		m_pHeader->set_client_steam_id( steamID.ConvertToUint64() );
}

CProtoBufNetPacket::~CProtoBufNetPacket()
{
	GProtoBufMsgMemoryPoolMgr()->FreeProtoBufHdr( m_pHeader );
	m_pNetPacket->Release();
}

bool CProtoBufNetPacket::GetMsgBody( const uint8*& pubData, uint32& cubData ) const
{
	if ( !m_bIsValid )
		return false;
	const uint32 cubHeader = sizeof( ProtoBufMsgHeader_t ) + GetFixedHeader().m_cubProtoBufExtHdr;
	pubData = PubData() + cubHeader;
	cubData = CubData() - cubHeader;
	return true;
}

//-----------------------------------------------------------------------------
// CProtoBufMsgBase
//-----------------------------------------------------------------------------
CProtoBufMsgBase::CProtoBufMsgBase()
	: m_pNetPacket( NULL )
	, m_pProtoBufHdr( GProtoBufMsgMemoryPoolMgr()->AllocProtoBufHdr() )
	, m_eMsg( 0 )
{
}

CProtoBufMsgBase::CProtoBufMsgBase( MsgType_t eMsgType )
	: m_pNetPacket( NULL )
	, m_pProtoBufHdr( GProtoBufMsgMemoryPoolMgr()->AllocProtoBufHdr() )
	, m_eMsg( eMsgType | k_EMsgProtoBufFlag )
{
}

CProtoBufMsgBase::~CProtoBufMsgBase()
{
	if ( m_pNetPacket )
		m_pNetPacket->Release();	// the header belongs to the packet
	else if ( m_pProtoBufHdr )
		GProtoBufMsgMemoryPoolMgr()->FreeProtoBufHdr( m_pProtoBufHdr );
}

bool CProtoBufMsgBase::InitFromPacket( IMsgNetPacket * pNetPacket )
{
	Assert( pNetPacket && pNetPacket->GetEMsgFormatType() == k_EMsgFormatTypeProtocolBuffer );
	if ( !pNetPacket || pNetPacket->GetEMsgFormatType() != k_EMsgFormatTypeProtocolBuffer )
		return false;
	CProtoBufNetPacket *pPacket = static_cast<CProtoBufNetPacket *>( pNetPacket );
	if ( !pPacket->IsValid() )
		return false;

	// The caller may pass the packet already owned by this message.
	pPacket->AddRef();
	if ( m_pNetPacket )
		m_pNetPacket->Release();
	else if ( m_pProtoBufHdr )
		GProtoBufMsgMemoryPoolMgr()->FreeProtoBufHdr( m_pProtoBufHdr );
	m_pNetPacket = pPacket;
	m_pProtoBufHdr = pPacket->GetProtoHeader();
	m_eMsg = pPacket->GetEMsg() | k_EMsgProtoBufFlag;

	const uint8 *pubBody = NULL;
	uint32 cubBody = 0;
	if ( !pPacket->GetMsgBody( pubBody, cubBody ) )
		return false;
	Message *pBody = GetGenericBody();
	return pBody && pBody->ParseFromArray( pubBody, cubBody );
}

uint8 *CProtoBufMsgBase::AllocateMessageMemory( MsgType_t eMsgType, const CMsgProtoBufHeader& hdr, uint32 cubBodySize, uint32* pCubTotalSizeOut )
{
	*pCubTotalSizeOut = 0;
	const int cubHeader = hdr.ByteSize();
	const uint64 cubTotal64 = sizeof( ProtoBufMsgHeader_t ) + (uint64)cubHeader + cubBodySize;
	// Protobuf's array API takes int lengths. Reject overflow before allocation
	// rather than allocating a wrapped size and copying a larger body into it.
	if ( cubHeader < 0 || cubTotal64 > INT_MAX || !hdr.IsInitialized() )
		return NULL;
	const uint32 cubTotal = (uint32)cubTotal64;
	uint8 *pubMemory = static_cast<uint8 *>( malloc( cubTotal ) );
	if ( !pubMemory )
		return NULL;
	new ( pubMemory ) ProtoBufMsgHeader_t( eMsgType, cubHeader );
	hdr.SerializeWithCachedSizesToArray( pubMemory + sizeof( ProtoBufMsgHeader_t ) );
	*pCubTotalSizeOut = cubTotal;
	return pubMemory;
}

void CProtoBufMsgBase::FreeMessageMemory( uint8* pMemory )
{
	free( pMemory );
}

bool CProtoBufMsgBase::BAsyncSend( IProtoBufSendHandler & pSender ) const
{
	Message *pBody = GetGenericBody();
	if ( !pBody )
		return false;
	return BAsyncSendProto( pSender, GetEMsg(), *m_pProtoBufHdr, *pBody );
}

bool CProtoBufMsgBase::BAsyncSendWithPreSerializedBody( IProtoBufSendHandler & pSender, const byte *pubBody, uint32 cubBody ) const
{
	return BAsyncSendWithPreSerializedBody( pSender, GetEMsg(), *m_pProtoBufHdr, pubBody, cubBody );
}

bool CProtoBufMsgBase::BAsyncSendWithPreSerializedBody( IProtoBufSendHandler& sender, MsgType_t eMsgType, const CMsgProtoBufHeader& hdr, const byte* pubBody, uint32 cubBody )
{
	if ( cubBody && !pubBody )
		return false;
	uint32 cubTotal = 0;
	uint8 *pubMessage = AllocateMessageMemory( eMsgType, hdr, cubBody, &cubTotal );
	if ( !pubMessage )
		return false;
	if ( cubBody )
		memcpy( pubMessage + cubTotal - cubBody, pubBody, cubBody );
	const bool bSent = sender.BAsyncSend( eMsgType, pubMessage, cubTotal );
	FreeMessageMemory( pubMessage );
	return bSent;
}

bool CProtoBufMsgBase::BAsyncSendProto( IProtoBufSendHandler& sender, MsgType_t eMsgType, const CMsgProtoBufHeader& hdr, const Message& proto )
{
	if ( !proto.IsInitialized() )
		return false;
	const uint32 cubBody = proto.ByteSize();
	uint32 cubTotal = 0;
	uint8 *pubMessage = AllocateMessageMemory( eMsgType, hdr, cubBody, &cubTotal );
	if ( !pubMessage )
		return false;
	proto.SerializeWithCachedSizesToArray( pubMessage + cubTotal - cubBody );
	const bool bSent = sender.BAsyncSend( eMsgType, pubMessage, cubTotal );
	FreeMessageMemory( pubMessage );
	return bSent;
}

} // namespace GCSDK
