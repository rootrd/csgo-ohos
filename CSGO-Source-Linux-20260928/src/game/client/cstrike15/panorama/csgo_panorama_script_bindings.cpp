//===================== Copyright (c) Valve Corporation. All Rights Reserved. ======================
//
//==================================================================================================
#include "cbase.h"

#include "uicomponents/uicomponent_common.h"
#include "csgo_panorama_script_bindings.h"
#include "vscript/ivscript.h"
#include "panorama/iuiengine.h"


v8::Local< v8::Array > JSArrayFromVector( v8::Isolate *pIsolate, const Vector &vec );
v8::Local< v8::Object > JSObjectFromKeyValues( v8::Isolate *pIsolate, KeyValues *pKeyValues );

//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
v8::Local< v8::Value > JSValueFromKeyValue( v8::Isolate *pIsolate, KeyValues *pValue )
{
	switch ( pValue->GetDataType() )
	{
		case KeyValues::TYPE_STRING:
		{
			return v8::String::NewFromUtf8( pIsolate, pValue->GetString() );
		}
		case KeyValues::TYPE_INT:
		{
			return v8::Int32::New( pIsolate, pValue->GetInt() );
		}
		case KeyValues::TYPE_FLOAT:
		{
			return v8::Number::New( pIsolate, pValue->GetFloat() );
		}
		case KeyValues::TYPE_UINT64:
		{
			char buf[32];
			V_sprintf_safe( buf, "%llu", pValue->GetUint64() );
			return v8::String::NewFromUtf8( pIsolate, buf );
		}

		case KeyValues::TYPE_WSTRING:
		{
			// Getstring() will switch and for WSTRING it does the wchar_t conversion to utf8
			// note the keyvalues has a buffer for conversion of 512 bytes
			return v8::String::NewFromUtf8( pIsolate, pValue->GetString() );
		}

		case KeyValues::TYPE_NONE:
		{
			return JSObjectFromKeyValues( pIsolate, pValue );
		}
		default:
		{
			Warning( "JSValueFromKeyValue failed to package parameter %s (type %d)\n", pValue->GetName(), pValue->GetDataType() );
			return v8::Null( pIsolate );
		}
	}
}

v8::Local< v8::Object > JSObjectFromKeyValues( v8::Isolate *pIsolate, KeyValues *pKeyValues )
{
	v8::Local< v8::Object > pObject = v8::Object::New( pIsolate );

	for ( KeyValues *val = pKeyValues->GetFirstSubKey(); val; val = val->GetNextKey() )
	{
		// avoid generating tons of new v8::local in loop
		// caveat: can't return any locals created inside HandleScope,
		// but we aren't doing that (we're just setting them into pObject)
		v8::HandleScope handleScope( pIsolate );

		v8::Local< v8::Value > pVal = JSValueFromKeyValue( pIsolate, val );
		if ( pVal->IsNull() )
			continue;

		v8::Local< v8::Value > pKey = v8::String::NewFromUtf8( pIsolate, val->GetName() );

		pObject->Set( pKey, pVal );
	}

	return pObject;
}


//--------------------------------------------------------------------------------------------------
//--------------------------------------------------------------------------------------------------
bool JSValueToVector( v8::Isolate *pIsolate, v8::Handle< v8::Value > val, Vector *pOutVec )
{
	pOutVec->Init();

	if ( val->IsArray() )
	{
		v8::Local< v8::Array > pJSArray = v8::Handle< v8::Array >::Cast( val );
		if ( pJSArray->Length() != 3 )
		{
			return false;
		}

		pOutVec->x = pJSArray->Get( 0 )->NumberValue();
		pOutVec->y = pJSArray->Get( 1 )->NumberValue();
		pOutVec->z = pJSArray->Get( 2 )->NumberValue();
		return true;
	}
	else if ( val->IsString() )
	{
		v8::String::Utf8Value strValue( val );
		V_StringToVector( pOutVec->Base(), *strValue );
		return true;
	}
	else if ( val->IsObject() )
	{
		v8::Local< v8::Object > pJSObject = v8::Handle< v8::Object >::Cast( val );
		pOutVec->x = pJSObject->Get( v8::String::NewFromUtf8( pIsolate, "x" ) )->NumberValue();
		pOutVec->y = pJSObject->Get( v8::String::NewFromUtf8( pIsolate, "y" ) )->NumberValue();
		pOutVec->z = pJSObject->Get( v8::String::NewFromUtf8( pIsolate, "z" ) )->NumberValue();
		return true;
	}
	else
	{
		return false;
	}
}

v8::Local< v8::Array > JSArrayFromVector( v8::Isolate *pIsolate, const Vector &vec )
{
	v8::Local< v8::Array > pArray = v8::Array::New( pIsolate, 3 );
    pArray->Set( 0, v8::Number::New( pIsolate, vec.x ) );
    pArray->Set( 1, v8::Number::New( pIsolate, vec.y ) );
    pArray->Set( 2, v8::Number::New( pIsolate, vec.z ) );
    return pArray;
}


void JSObjectToKeyValues( v8::Handle< v8::Object > pObject, KeyValues *pTargetKV );

bool JSValueToKeyValues( v8::Handle< v8::Value > val, KeyValues *pTargetKV )
{
	if ( val->IsString() )
	{
		v8::String::Utf8Value valueStr( val );
		pTargetKV->SetString( NULL, *valueStr );
	}
	else if ( val->IsBoolean() )
	{
		pTargetKV->SetBool( NULL, val->BooleanValue() );
	}
	else if ( val->IsInt32() )
	{
		pTargetKV->SetInt( NULL, val->Int32Value() );
	}
	else if ( val->IsArray() )
	{
		v8::Local< v8::Array > pJSArray = v8::Handle< v8::Array >::Cast( val );
		for ( uint32 iArr = 0; iArr < pJSArray->Length(); ++iArr )
		{
			KeyValues *pArrayItem = new KeyValues( CFmtStr("%d",iArr).Get() );
			JSValueToKeyValues( pJSArray->Get( iArr ), pArrayItem );
			pTargetKV->AddSubKey( pArrayItem );
		}
	}
	else if ( val->IsNumber() )
	{
		pTargetKV->SetFloat( NULL, val->NumberValue() );
	}
	else if ( val->IsObject() )
	{
		JSObjectToKeyValues( v8::Handle< v8::Object >::Cast( val ), pTargetKV );
	}
	else
	{
		return false;
	}
	
	return true;
}

void JSObjectToKeyValues( v8::Handle< v8::Object > pObject, KeyValues *pTargetKV )
{
	v8::Local< v8::Array > keys = pObject->GetPropertyNames();
	for( uint32_t iProperty = 0; iProperty < keys->Length(); ++iProperty )
	{
		v8::Local< v8::String > key = v8::Local< v8::String >::Cast( keys->Get( iProperty ) );
		v8::Local< v8::Value > val = pObject->Get( key );
		v8::String::Utf8Value keyName( key );

		KeyValues *pSubKey = new KeyValues( *keyName );
		if ( !JSValueToKeyValues( val, pSubKey ) )
		{
			v8::String::Utf8Value strType( val->TypeOf( panorama::GetV8Isolate() ) );
			v8::String::Utf8Value strName( key );

			AssertMsg( false, CFmtStr( "Unrecognized JS type '%s' for key '%s' in JSValueToKeyValues\n", *strType ? *strType : "<unknown>", *strName ).Get() );
		}
		pTargetKV->AddSubKey( pSubKey );
	}
}

KeyValues *JSObjectToKeyValues( v8::Handle< v8::Object > pObject )
{
	KeyValues *pKV = new KeyValues("");
	JSObjectToKeyValues( pObject, pKV );
	return pKV;
}

void V8ParamToPanoramaType( const v8::Handle<v8::Value> &pValueIn, JSObjectAsKeyValues *out )
{
	// uses move assignment to avoid KV copy
	*out = JSObjectAsKeyValues( JSObjectToKeyValues( v8::Handle<v8::Object>::Cast( pValueIn ) ) );
}

void PanoramaTypeToV8Param( JSObjectAsKeyValues &pIn, v8::Handle<v8::Value> *pValueOut )
{
	*pValueOut = JSObjectFromKeyValues( panorama::GetV8Isolate(), pIn );
}

// Helper functions for setting JSO data
//
//
v8::Local< v8::Object > CreateJSOSubObject( v8::Local< v8::Object > pObject, const char *key )
{
	v8::Local< v8::Object > pSubObject = v8::Object::New( panorama::GetV8Isolate() );

	pObject->Set( v8::String::NewFromUtf8( panorama::GetV8Isolate(), key ), pSubObject );
	return pSubObject;
}

void CreateJSOEntry_Number( v8::Local< v8::Object > pObject, const char *key, int value )
{
	pObject->Set( v8::String::NewFromUtf8( panorama::GetV8Isolate(), key ), v8::Number::New( panorama::GetV8Isolate(), value ) );
}


void CreateJSOEntry_String( v8::Local< v8::Object > pObject, const char *key, const char * value )
{
	pObject->Set( v8::String::NewFromUtf8( panorama::GetV8Isolate(), key ), v8::String::NewFromUtf8( panorama::GetV8Isolate(), value ) );
}

void CreateJSOEntry_Bool( v8::Local< v8::Object > pObject, const char *key, bool value )
{
	pObject->Set( v8::String::NewFromUtf8( panorama::GetV8Isolate(), key ), v8::Boolean::New( panorama::GetV8Isolate(), value ) );
}

