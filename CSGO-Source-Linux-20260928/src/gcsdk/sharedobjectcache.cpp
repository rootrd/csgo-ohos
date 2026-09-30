//====== Copyright (c), Valve Corporation, All rights reserved. =======
//
// Purpose: Per-type and per-owner shared object containers. Objects of one
//			type are kept sorted by key so lookups are binary searches.
//
//=============================================================================

#include "gcsdk/gcclientsdk.h"
#include "tier0/memdbgon.h"

namespace GCSDK
{

//-----------------------------------------------------------------------------
// CSharedObjectTypeCache
//-----------------------------------------------------------------------------
CSharedObjectTypeCache::CSharedObjectTypeCache( int nTypeID )
	: m_nTypeID( nTypeID )
{
}

CSharedObjectTypeCache::~CSharedObjectTypeCache()
{
	// Ownership of the objects belongs to the derived cache (see
	// CGCClientSharedObjectTypeCache::RemoveAllObjects); nothing is freed here.
}

int CSharedObjectTypeCache::FindSharedObjectIndex( const CSharedObject & soIndex ) const
{
	int nLow = 0, nHigh = m_vecObjects.Count() - 1;
	while ( nLow <= nHigh )
	{
		const int nMid = ( nLow + nHigh ) / 2;
		const CSharedObject *pObject = m_vecObjects[nMid];
		if ( pObject->BIsKeyLess( soIndex ) )
			nLow = nMid + 1;
		else if ( soIndex.BIsKeyLess( *pObject ) )
			nHigh = nMid - 1;
		else
			return nMid;
	}
	return -1;
}

bool CSharedObjectTypeCache::AddObject( CSharedObject *pObject )
{
	Assert( pObject && pObject->GetTypeID() == m_nTypeID );
	if ( !pObject || pObject->GetTypeID() != m_nTypeID )
		return false;
	// Keep the vector ordered; refuse duplicates instead of shadowing an entry.
	int nInsert = 0, nHigh = m_vecObjects.Count();
	while ( nInsert < nHigh )
	{
		const int nMid = ( nInsert + nHigh ) / 2;
		if ( m_vecObjects[nMid]->BIsKeyLess( *pObject ) )
			nInsert = nMid + 1;
		else
			nHigh = nMid;
	}
	if ( nInsert < m_vecObjects.Count() && m_vecObjects[nInsert]->BIsKeyEqual( *pObject ) )
	{
		AssertMsg1( false, "Shared object of type %s added twice", CSharedObject::PchClassName( m_nTypeID ) );
		return false;
	}
	m_vecObjects.InsertBefore( nInsert, pObject );
	return true;
}

bool CSharedObjectTypeCache::AddObjectClean( CSharedObject *pObject )
{
	// "Clean" only affects GC-side dirty tracking; the client container is identical.
	return AddObject( pObject );
}

CSharedObject *CSharedObjectTypeCache::RemoveObject( const CSharedObject & soIndex )
{
	const int nIndex = FindSharedObjectIndex( soIndex );
	return nIndex >= 0 ? RemoveObjectByIndex( nIndex ) : NULL;
}

CSharedObject *CSharedObjectTypeCache::RemoveObjectByIndex( uint32 nObj )
{
	if ( nObj >= (uint32)m_vecObjects.Count() )
		return NULL;
	CSharedObject *pObject = m_vecObjects[nObj];
	m_vecObjects.Remove( nObj );
	return pObject;
}

void CSharedObjectTypeCache::RemoveAllObjectsWithoutDeleting()
{
	m_vecObjects.RemoveAll();
}

void CSharedObjectTypeCache::EnsureCapacity( uint32 nItems )
{
	m_vecObjects.EnsureCapacity( nItems );
}

CSharedObject *CSharedObjectTypeCache::FindSharedObject( const CSharedObject & soIndex )
{
	const int nIndex = FindSharedObjectIndex( soIndex );
	return nIndex >= 0 ? m_vecObjects[nIndex] : NULL;
}

const CSharedObject *CSharedObjectTypeCache::FindSharedObject( const CSharedObject & soIndex ) const
{
	const int nIndex = FindSharedObjectIndex( soIndex );
	return nIndex >= 0 ? m_vecObjects[nIndex] : NULL;
}

void CSharedObjectTypeCache::Dump() const
{
	Msg( "  %s: %u objects\n", CSharedObject::PchClassName( m_nTypeID ), GetCount() );
	for ( int i = 0; i < m_vecObjects.Count(); ++i )
		m_vecObjects[i]->Dump();
}

//-----------------------------------------------------------------------------
// CSharedObjectCache
//-----------------------------------------------------------------------------
CSharedObjectCache::CSharedObjectCache()
	: m_ulVersion( 0 )
{
}

CSharedObjectCache::~CSharedObjectCache()
{
	m_CacheObjects.PurgeAndDeleteElements();
}

bool CSharedObjectCache::AddObject( CSharedObject *pSharedObject )
{
	if ( !pSharedObject )
		return false;
	return CreateBaseTypeCache( pSharedObject->GetTypeID() )->AddObject( pSharedObject );
}

bool CSharedObjectCache::AddObjectClean( CSharedObject *pSharedObject )
{
	if ( !pSharedObject )
		return false;
	return CreateBaseTypeCache( pSharedObject->GetTypeID() )->AddObjectClean( pSharedObject );
}

CSharedObject *CSharedObjectCache::RemoveObject( const CSharedObject & soIndex )
{
	CSharedObjectTypeCache *pTypeCache = FindBaseTypeCache( soIndex.GetTypeID() );
	return pTypeCache ? pTypeCache->RemoveObject( soIndex ) : NULL;
}

bool CSharedObjectCache::RemoveAllObjectsWithoutDeleting()
{
	for ( int i = 0; i < m_CacheObjects.Count(); ++i )
		m_CacheObjects[i]->RemoveAllObjectsWithoutDeleting();
	return true;
}

const CSharedObjectTypeCache *CSharedObjectCache::FindBaseTypeCache( int nClassID ) const
{
	for ( int i = 0; i < m_CacheObjects.Count(); ++i )
		if ( m_CacheObjects[i]->GetTypeID() == nClassID )
			return m_CacheObjects[i];
	return NULL;
}

CSharedObjectTypeCache *CSharedObjectCache::FindBaseTypeCache( int nClassID )
{
	for ( int i = 0; i < m_CacheObjects.Count(); ++i )
		if ( m_CacheObjects[i]->GetTypeID() == nClassID )
			return m_CacheObjects[i];
	return NULL;
}

CSharedObjectTypeCache *CSharedObjectCache::CreateBaseTypeCache( int nClassID )
{
	if ( CSharedObjectTypeCache *pExisting = FindBaseTypeCache( nClassID ) )
		return pExisting;
	CSharedObjectTypeCache *pTypeCache = AllocateTypeCache( nClassID );
	m_CacheObjects.AddToTail( pTypeCache );
	return pTypeCache;
}

CSharedObject *CSharedObjectCache::FindSharedObject( const CSharedObject & soIndex )
{
	CSharedObjectTypeCache *pTypeCache = FindBaseTypeCache( soIndex.GetTypeID() );
	return pTypeCache ? pTypeCache->FindSharedObject( soIndex ) : NULL;
}

const CSharedObject *CSharedObjectCache::FindSharedObject( const CSharedObject & soIndex ) const
{
	const CSharedObjectTypeCache *pTypeCache = FindBaseTypeCache( soIndex.GetTypeID() );
	return pTypeCache ? pTypeCache->FindSharedObject( soIndex ) : NULL;
}

void CSharedObjectCache::Dump() const
{
	const SOID_t owner = GetOwner();
	Msg( "Shared object cache owner type %u id %llu, version %llu, %d types\n", owner.m_type,
		(unsigned long long)owner.m_id, (unsigned long long)m_ulVersion, m_CacheObjects.Count() );
	for ( int i = 0; i < m_CacheObjects.Count(); ++i )
		m_CacheObjects[i]->Dump();
}

} // namespace GCSDK
