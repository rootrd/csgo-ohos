//====== Copyright (c), Valve Corporation, All rights reserved. =======
//
// Purpose: CSharedObject type registry and key comparison helpers.
//
//=============================================================================

#include "gcsdk/gcclientsdk.h"
#include "tier0/memdbgon.h"

namespace GCSDK
{

CSharedObject::TVecFactories CSharedObject::sm_vecFactories;

bool CSharedObject::BIsKeyEqual( const CSharedObject & soRHS ) const
{
	return !BIsKeyLess( soRHS ) && !soRHS.BIsKeyLess( *this );
}

const CSharedObject::SharedObjectInfo_t *CSharedObject::FindSharedObjectInfo( int nTypeID )
{
	// Sorted by type id; the registry stays small (one entry per shared object class).
	int nLow = 0, nHigh = sm_vecFactories.Count() - 1;
	while ( nLow <= nHigh )
	{
		const int nMid = ( nLow + nHigh ) / 2;
		const SharedObjectInfo_t &info = sm_vecFactories[nMid];
		if ( info.m_nID == nTypeID )
			return &info;
		if ( info.m_nID < nTypeID )
			nLow = nMid + 1;
		else
			nHigh = nMid - 1;
	}
	return NULL;
}

void CSharedObject::RegisterFactory( int nTypeID, SOCreationFunc_t fnFactory, uint32 unFlags, const char *pchClassName,
	const char *pszBuildCacheName, const char *pszCreateName, const char *pszUpdateName )
{
	if ( FindSharedObjectInfo( nTypeID ) )
	{
		AssertMsg2( false, "Shared object type %d (%s) registered twice", nTypeID, pchClassName );
		return;
	}
	SharedObjectInfo_t info;
	info.m_nID = nTypeID;
	info.m_unFlags = unFlags;
	info.m_pFactoryFunction = fnFactory;
	info.m_pchClassName = pchClassName;
	info.m_pchBuildCacheSubNodeName = pszBuildCacheName;
	info.m_pchCreateNodeName = pszCreateName;
	info.m_pchUpdateNodeName = pszUpdateName;
	sm_vecFactories.Insert( info );
}

CSharedObject *CSharedObject::Create( int nTypeID )
{
	const SharedObjectInfo_t *pInfo = FindSharedObjectInfo( nTypeID );
	if ( !pInfo || !pInfo->m_pFactoryFunction )
	{
		AssertMsg1( false, "No shared object factory registered for type %d", nTypeID );
		return NULL;
	}
	return pInfo->m_pFactoryFunction();
}

uint32 CSharedObject::GetTypeFlags( int nTypeID )
{
	const SharedObjectInfo_t *pInfo = FindSharedObjectInfo( nTypeID );
	return pInfo ? pInfo->m_unFlags : 0;
}

const char *CSharedObject::PchClassName( int nTypeID )
{
	const SharedObjectInfo_t *pInfo = FindSharedObjectInfo( nTypeID );
	return pInfo ? pInfo->m_pchClassName : "<unregistered shared object>";
}

const char *CSharedObject::PchClassBuildCacheNodeName( int nTypeID )
{
	const SharedObjectInfo_t *pInfo = FindSharedObjectInfo( nTypeID );
	return pInfo ? pInfo->m_pchBuildCacheSubNodeName : "<unregistered shared object>";
}

const char *CSharedObject::PchClassCreateNodeName( int nTypeID )
{
	const SharedObjectInfo_t *pInfo = FindSharedObjectInfo( nTypeID );
	return pInfo ? pInfo->m_pchCreateNodeName : "<unregistered shared object>";
}

const char *CSharedObject::PchClassUpdateNodeName( int nTypeID )
{
	const SharedObjectInfo_t *pInfo = FindSharedObjectInfo( nTypeID );
	return pInfo ? pInfo->m_pchUpdateNodeName : "<unregistered shared object>";
}

} // namespace GCSDK
