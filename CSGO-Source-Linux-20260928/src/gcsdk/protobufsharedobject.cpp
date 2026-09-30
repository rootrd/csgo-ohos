//====== Copyright (c), Valve Corporation, All rights reserved. =======
//
// Purpose: Shared objects whose state is a protobuf message. Key fields are
//			the message fields tagged with the (key_field) option in the proto.
//
//=============================================================================

#include "gcsdk/gcclientsdk.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "tier0/memdbgon.h"

namespace GCSDK
{

using ::google::protobuf::Descriptor;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::Reflection;

static bool BIsKeyField( const FieldDescriptor *pField )
{
	return pField->options().GetExtension( key_field );
}

// Three-way comparison of one singular field in two messages of the same type.
static int CompareField( const Message &msgLHS, const Message &msgRHS, const FieldDescriptor *pField )
{
	const Reflection *pReflection = msgLHS.GetReflection();
	if ( pField->is_repeated() )
	{
		AssertMsg1( false, "Repeated field %s cannot be a shared object key", pField->full_name().c_str() );
		return 0;
	}
	switch ( pField->cpp_type() )
	{
	case FieldDescriptor::CPPTYPE_INT32:
	{
		const int32 lhs = pReflection->GetInt32( msgLHS, pField ), rhs = pReflection->GetInt32( msgRHS, pField );
		return lhs < rhs ? -1 : lhs > rhs;
	}
	case FieldDescriptor::CPPTYPE_UINT32:
	{
		const uint32 lhs = pReflection->GetUInt32( msgLHS, pField ), rhs = pReflection->GetUInt32( msgRHS, pField );
		return lhs < rhs ? -1 : lhs > rhs;
	}
	case FieldDescriptor::CPPTYPE_INT64:
	{
		const int64 lhs = pReflection->GetInt64( msgLHS, pField ), rhs = pReflection->GetInt64( msgRHS, pField );
		return lhs < rhs ? -1 : lhs > rhs;
	}
	case FieldDescriptor::CPPTYPE_UINT64:
	{
		const uint64 lhs = pReflection->GetUInt64( msgLHS, pField ), rhs = pReflection->GetUInt64( msgRHS, pField );
		return lhs < rhs ? -1 : lhs > rhs;
	}
	case FieldDescriptor::CPPTYPE_BOOL:
	{
		const bool lhs = pReflection->GetBool( msgLHS, pField ), rhs = pReflection->GetBool( msgRHS, pField );
		return lhs < rhs ? -1 : lhs > rhs;
	}
	case FieldDescriptor::CPPTYPE_ENUM:
	{
		const int lhs = pReflection->GetEnum( msgLHS, pField )->number(), rhs = pReflection->GetEnum( msgRHS, pField )->number();
		return lhs < rhs ? -1 : lhs > rhs;
	}
	case FieldDescriptor::CPPTYPE_FLOAT:
	{
		const float lhs = pReflection->GetFloat( msgLHS, pField ), rhs = pReflection->GetFloat( msgRHS, pField );
		return lhs < rhs ? -1 : lhs > rhs;
	}
	case FieldDescriptor::CPPTYPE_DOUBLE:
	{
		const double lhs = pReflection->GetDouble( msgLHS, pField ), rhs = pReflection->GetDouble( msgRHS, pField );
		return lhs < rhs ? -1 : lhs > rhs;
	}
	case FieldDescriptor::CPPTYPE_STRING:
		return pReflection->GetString( msgLHS, pField ).compare( pReflection->GetString( msgRHS, pField ) );
	default:
		AssertMsg1( false, "Field %s has a type that cannot be a shared object key", pField->full_name().c_str() );
		return 0;
	}
}

bool CProtoBufSharedObjectBase::BParseFromMessage( const CUtlBuffer & buffer )
{
	return GetPObject()->ParseFromArray( buffer.Base(), buffer.TellMaxPut() );
}

bool CProtoBufSharedObjectBase::BParseFromMessage( const std::string &buffer )
{
	return GetPObject()->ParseFromString( buffer );
}

bool CProtoBufSharedObjectBase::BUpdateFromNetwork( const CSharedObject & objUpdate )
{
	Assert( GetTypeID() == objUpdate.GetTypeID() );
	const Message *pUpdate = static_cast<const CProtoBufSharedObjectBase &>( objUpdate ).GetPObject();
	GetPObject()->CopyFrom( *pUpdate );
	return true;
}

bool CProtoBufSharedObjectBase::BAddToMessage( std::string *pBuffer ) const
{
	return GetPObject()->SerializeToString( pBuffer );
}

bool CProtoBufSharedObjectBase::BAddDestroyToMessage( std::string *pBuffer ) const
{
	Message *pDestroy = BuildDestroyToMessage( *GetPObject() );
	const bool bResult = pDestroy->SerializeToString( pBuffer );
	delete pDestroy;
	return bResult;
}

bool CProtoBufSharedObjectBase::BIsKeyLess( const CSharedObject & soRHS ) const
{
	Assert( GetTypeID() == soRHS.GetTypeID() );
	const Message &msgLHS = *GetPObject();
	const Message &msgRHS = *static_cast<const CProtoBufSharedObjectBase &>( soRHS ).GetPObject();
	const Descriptor *pDescriptor = msgLHS.GetDescriptor();
	for ( int i = 0; i < pDescriptor->field_count(); ++i )
	{
		const FieldDescriptor *pField = pDescriptor->field( i );
		if ( !BIsKeyField( pField ) )
			continue;
		const int nCompare = CompareField( msgLHS, msgRHS, pField );
		if ( nCompare != 0 )
			return nCompare < 0;
	}
	return false;
}

void CProtoBufSharedObjectBase::Copy( const CSharedObject & soRHS )
{
	Assert( GetTypeID() == soRHS.GetTypeID() );
	GetPObject()->CopyFrom( *static_cast<const CProtoBufSharedObjectBase &>( soRHS ).GetPObject() );
}

void CProtoBufSharedObjectBase::Dump() const
{
	Dump( *GetPObject() );
}

bool CProtoBufSharedObjectBase::SerializeToBuffer( const Message & msg, CUtlBuffer & bufOutput )
{
	const int nBytes = msg.ByteSize();
	bufOutput.EnsureCapacity( bufOutput.TellPut() + nBytes );
	if ( !msg.SerializeWithCachedSizesToArray( static_cast<uint8 *>( bufOutput.PeekPut() ) ) )
		return false;
	bufOutput.SeekPut( CUtlBuffer::SEEK_CURRENT, nBytes );
	return true;
}

void CProtoBufSharedObjectBase::Dump( const Message & msg )
{
	Msg( "%s\n%s", msg.GetTypeName().c_str(), msg.DebugString().c_str() );
}

// A destroy message carries only the key so the receiver can find the object.
Message *CProtoBufSharedObjectBase::BuildDestroyToMessage( const Message & msg )
{
	Message *pDestroy = msg.New();
	pDestroy->CopyFrom( msg );
	const Descriptor *pDescriptor = msg.GetDescriptor();
	const Reflection *pReflection = pDestroy->GetReflection();
	for ( int i = 0; i < pDescriptor->field_count(); ++i )
	{
		const FieldDescriptor *pField = pDescriptor->field( i );
		if ( !BIsKeyField( pField ) )
			pReflection->ClearField( pDestroy, pField );
	}
	return pDestroy;
}

} // namespace GCSDK
