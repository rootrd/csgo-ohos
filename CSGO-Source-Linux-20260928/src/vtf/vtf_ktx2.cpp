//========= Copyright (c) 2026, CSGO-Source-Linux port ========================
//
// Purpose: Rebuilds VTF file images from the offline ASTC texture pack.
//
// scripts/astc-convert stores every pak01 .vtf as a .ktx2 whose key/value data
// keeps the original VTF header ("source.vtf.header") and each resource payload
// ("source.vtf.resource.<type>"). Only the image payload is re-encoded. Putting
// the header, resources and KTX2 levels back into VTF file order lets
// CVTFTexture::Unserialize and CTexture load pack textures exactly like a .vtf,
// including mip skipping, animation frames, cube faces, sheets and LOD settings.
//
//=============================================================================

#include "vtf/vtf.h"
#include "tier1/strtools.h"
#include "tier1/utlbuffer.h"

// NOTE: This has to be the last file included!
#include "tier0/memdbgon.h"

namespace
{

// Vulkan format values stored in the KTX2 header (vulkan_core.h).
enum
{
	KTX_VK_FORMAT_R8G8_SNORM = 17,
	KTX_VK_FORMAT_R8G8B8A8_UNORM = 37,
	KTX_VK_FORMAT_R8G8B8A8_SNORM = 38,
	KTX_VK_FORMAT_R16G16_SFLOAT = 83,
	KTX_VK_FORMAT_R16G16B16A16_UNORM = 91,
	KTX_VK_FORMAT_R16G16B16A16_SFLOAT = 97,
	KTX_VK_FORMAT_R32_SFLOAT = 100,
	KTX_VK_FORMAT_R32G32_SFLOAT = 103,
	KTX_VK_FORMAT_R32G32B32_SFLOAT = 106,
	KTX_VK_FORMAT_R32G32B32A32_SFLOAT = 109,
	KTX_VK_FORMAT_ASTC_4x4_UNORM_BLOCK = 157,
	KTX_VK_FORMAT_ASTC_6x6_UNORM_BLOCK = 165,
};

// KTX2 header (KTX File Format Specification 2.0, section 3).
const int KTX2_LEVEL_INDEX_OFFSET = 80;
const unsigned char s_KTX2Identifier[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };

// VTF disk header offsets (see VTFFileHeader_t; the file layout is packed).
const int VTF_OFFSET_HEADER_SIZE = 12;
const int VTF_OFFSET_WIDTH = 16;
const int VTF_OFFSET_HEIGHT = 18;
const int VTF_OFFSET_FLAGS = 20;
const int VTF_OFFSET_FRAMES = 24;
const int VTF_OFFSET_IMAGE_FORMAT = 52;
const int VTF_OFFSET_MIP_COUNT = 56;
const int VTF_OFFSET_LOW_RES_FORMAT = 57;
const int VTF_OFFSET_LOW_RES_WIDTH = 61;
const int VTF_OFFSET_LOW_RES_HEIGHT = 62;
const int VTF_OFFSET_DEPTH = 63;
const int VTF_OFFSET_RESOURCE_COUNT = 68;
const int VTF_OFFSET_RESOURCES = 80;
const int VTF_MAX_RESOURCES = 32;

inline uint32 Read32( const unsigned char *p )
{
	uint32 v;
	memcpy( &v, p, sizeof( v ) );
	return LittleDWord( v );
}

inline uint16 Read16( const unsigned char *p )
{
	uint16 v;
	memcpy( &v, p, sizeof( v ) );
	return LittleShort( v );
}

inline uint64 Read64( const unsigned char *p )
{
	return uint64( Read32( p ) ) | ( uint64( Read32( p + 4 ) ) << 32 );
}

inline void Write32( unsigned char *p, uint32 v )
{
	v = LittleDWord( v );
	memcpy( p, &v, sizeof( v ) );
}

inline bool RangeFits( uint64 nOffset, uint64 nLength, uint64 nSize )
{
	return nOffset <= nSize && nLength <= nSize - nOffset;
}

struct KeyValue_t
{
	const char *m_pKey;
	const unsigned char *m_pValue;
	uint32 m_nValueLength;
};

class CKTX2ToVTF
{
public:
	CKTX2ToVTF( const unsigned char *pData, int nSize, char *pError, int nErrorSize ) :
		m_pData( pData ), m_nSize( nSize ), m_pError( pError ), m_nErrorSize( nErrorSize ), m_nKeyValues( 0 )
	{
	}

	bool Convert( CUtlBuffer &vtf );

private:
	bool Fail( const char *pFormat, ... ) FMTFUNCTION( 2, 3 );
	bool ParseKeyValues( uint32 nOffset, uint32 nLength );
	const KeyValue_t *FindKeyValue( const char *pKey ) const;
	bool MapImageFormat( uint32 nVkFormat, ImageFormat nSourceFormat, int nDepth, ImageFormat *pFormat );
	bool PutResource( CUtlBuffer &vtf, uint32 nType, int nExpectedLength, uint32 *pOffset );

	const unsigned char *m_pData;
	int m_nSize;
	char *m_pError;
	int m_nErrorSize;
	KeyValue_t m_KeyValues[64];
	int m_nKeyValues;
};

bool CKTX2ToVTF::Fail( const char *pFormat, ... )
{
	va_list args;
	va_start( args, pFormat );
	V_vsnprintf( m_pError, m_nErrorSize, pFormat, args );
	va_end( args );
	return false;
}

bool CKTX2ToVTF::ParseKeyValues( uint32 nOffset, uint32 nLength )
{
	if ( !RangeFits( nOffset, nLength, m_nSize ) )
		return Fail( "key/value data out of range" );

	const unsigned char *p = m_pData + nOffset;
	const unsigned char *pEnd = p + nLength;
	while ( p < pEnd )
	{
		if ( pEnd - p < 4 )
			return Fail( "truncated key/value entry" );
		uint32 nEntryLength = Read32( p );
		p += 4;
		if ( nEntryLength > uint32( pEnd - p ) )
			return Fail( "truncated key/value entry" );

		const unsigned char *pNul = (const unsigned char *)memchr( p, 0, nEntryLength );
		if ( !pNul || pNul == p )
			return Fail( "malformed key/value key" );
		if ( m_nKeyValues == ARRAYSIZE( m_KeyValues ) )
			return Fail( "too many key/value entries" );

		KeyValue_t &kv = m_KeyValues[m_nKeyValues++];
		kv.m_pKey = (const char *)p;
		kv.m_pValue = pNul + 1;
		kv.m_nValueLength = uint32( p + nEntryLength - kv.m_pValue );
		if ( FindKeyValue( kv.m_pKey ) != &kv )
			return Fail( "duplicate key \"%s\"", kv.m_pKey );

		// Entries are padded to 4 bytes.
		p += ( nEntryLength + 3 ) & ~3u;
	}
	return true;
}

const KeyValue_t *CKTX2ToVTF::FindKeyValue( const char *pKey ) const
{
	for ( int i = 0; i < m_nKeyValues; ++i )
	{
		if ( !V_strcmp( m_KeyValues[i].m_pKey, pKey ) )
			return &m_KeyValues[i];
	}
	return NULL;
}

bool CKTX2ToVTF::MapImageFormat( uint32 nVkFormat, ImageFormat nSourceFormat, int nDepth, ImageFormat *pFormat )
{
	// Lossless entries are the source bytes unchanged, so the VTF format stays.
	ImageFormat nLosslessFormat = IMAGE_FORMAT_UNKNOWN;
	switch ( nVkFormat )
	{
	case KTX_VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
		*pFormat = IMAGE_FORMAT_ASTC_4X4;
		return true;
	case KTX_VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
		*pFormat = IMAGE_FORMAT_ASTC_6X6;
		return true;
	case KTX_VK_FORMAT_R8G8B8A8_UNORM:
		// Volume textures are decoded to RGBA8; ASTC 2D blocks cannot hold them.
		if ( nDepth <= 1 )
			return Fail( "RGBA8 payload is only produced for volume textures" );
		*pFormat = IMAGE_FORMAT_RGBA8888;
		return true;
	case KTX_VK_FORMAT_R8G8_SNORM:				nLosslessFormat = IMAGE_FORMAT_UV88; break;
	case KTX_VK_FORMAT_R8G8B8A8_SNORM:			nLosslessFormat = IMAGE_FORMAT_UVWQ8888; break;
	case KTX_VK_FORMAT_R16G16_SFLOAT:			nLosslessFormat = IMAGE_FORMAT_RG1616F; break;
	case KTX_VK_FORMAT_R16G16B16A16_UNORM:		nLosslessFormat = IMAGE_FORMAT_RGBA16161616; break;
	case KTX_VK_FORMAT_R16G16B16A16_SFLOAT:		nLosslessFormat = IMAGE_FORMAT_RGBA16161616F; break;
	case KTX_VK_FORMAT_R32_SFLOAT:				nLosslessFormat = IMAGE_FORMAT_R32F; break;
	case KTX_VK_FORMAT_R32G32_SFLOAT:			nLosslessFormat = IMAGE_FORMAT_RG3232F; break;
	case KTX_VK_FORMAT_R32G32B32_SFLOAT:		nLosslessFormat = IMAGE_FORMAT_RGB323232F; break;
	case KTX_VK_FORMAT_R32G32B32A32_SFLOAT:		nLosslessFormat = IMAGE_FORMAT_RGBA32323232F; break;
	default:
		// sRGB ASTC blocks (vtf2astc --srgb) would bypass the material-selected sRGB state.
		return Fail( "unsupported VkFormat %u", nVkFormat );
	}
	if ( nLosslessFormat != nSourceFormat )
		return Fail( "VkFormat %u does not match source VTF format %d", nVkFormat, nSourceFormat );
	*pFormat = nLosslessFormat;
	return true;
}

// Appends one auxiliary resource from key/value data and returns its file offset.
bool CKTX2ToVTF::PutResource( CUtlBuffer &vtf, uint32 nType, int nExpectedLength, uint32 *pOffset )
{
	char pKey[64];
	V_snprintf( pKey, sizeof( pKey ), "source.vtf.resource.%08x", nType );
	const KeyValue_t *pKV = FindKeyValue( pKey );
	if ( !pKV )
		return Fail( "missing %s", pKey );

	// Low-res images are raw; other chunks keep their int32 length prefix.
	uint32 nLength = pKV->m_nValueLength;
	bool bValid = ( nExpectedLength >= 0 ) ? ( nLength == uint32( nExpectedLength ) ) :
		( nLength >= 4 && Read32( pKV->m_pValue ) == nLength - 4 );
	if ( !bValid )
		return Fail( "%s has an invalid length (%u)", pKey, nLength );

	*pOffset = vtf.TellPut();
	vtf.Put( pKV->m_pValue, nLength );
	return true;
}

bool CKTX2ToVTF::Convert( CUtlBuffer &vtf )
{
	if ( m_nSize < KTX2_LEVEL_INDEX_OFFSET || memcmp( m_pData, s_KTX2Identifier, sizeof( s_KTX2Identifier ) ) )
		return Fail( "not a KTX2 file" );

	const uint32 nVkFormat = Read32( m_pData + 12 );
	const uint32 nPixelWidth = Read32( m_pData + 20 );
	const uint32 nPixelHeight = Read32( m_pData + 24 );
	const uint32 nPixelDepth = Read32( m_pData + 28 );
	const uint32 nLayerCount = Read32( m_pData + 32 );
	const uint32 nFaceCount = Read32( m_pData + 36 );
	const uint32 nLevelCount = Read32( m_pData + 40 );
	const uint32 nSupercompression = Read32( m_pData + 44 );
	const uint32 nKVDOffset = Read32( m_pData + 56 );
	const uint32 nKVDLength = Read32( m_pData + 60 );

	if ( nSupercompression != 0 )
		return Fail( "supercompression scheme %u is not supported", nSupercompression );
	if ( nLevelCount == 0 || nLevelCount > 16 )
		return Fail( "invalid level count %u", nLevelCount );
	if ( !RangeFits( KTX2_LEVEL_INDEX_OFFSET, uint64( nLevelCount ) * 24, m_nSize ) )
		return Fail( "truncated level index" );
	if ( !ParseKeyValues( nKVDOffset, nKVDLength ) )
		return false;

	// The original VTF header, including its resource dictionary.
	const KeyValue_t *pHeaderKV = FindKeyValue( "source.vtf.header" );
	if ( !pHeaderKV )
		return Fail( "missing source.vtf.header" );
	const unsigned char *pHeader = pHeaderKV->m_pValue;
	const uint32 nHeaderSize = pHeaderKV->m_nValueLength;
	if ( nHeaderSize < 63 || memcmp( pHeader, "VTF", 4 ) || Read32( pHeader + 4 ) != VTF_MAJOR_VERSION )
		return Fail( "source.vtf.header is not a VTF 7 header" );
	const uint32 nMinorVersion = Read32( pHeader + 8 );
	const uint32 nMinimumHeader = ( nMinorVersion >= 3 ) ? VTF_OFFSET_RESOURCES : ( nMinorVersion == 2 ) ? 65 : 63;
	if ( nMinorVersion > VTF_MINOR_VERSION || nHeaderSize < nMinimumHeader || Read32( pHeader + VTF_OFFSET_HEADER_SIZE ) != nHeaderSize )
		return Fail( "unsupported VTF header 7.%u (%u bytes)", nMinorVersion, nHeaderSize );

	const int nWidth = Read16( pHeader + VTF_OFFSET_WIDTH );
	const int nHeight = Read16( pHeader + VTF_OFFSET_HEIGHT );
	const uint32 nFlags = Read32( pHeader + VTF_OFFSET_FLAGS );
	const int nFrames = Read16( pHeader + VTF_OFFSET_FRAMES );
	const ImageFormat nSourceFormat = (ImageFormat)Read32( pHeader + VTF_OFFSET_IMAGE_FORMAT );
	const uint32 nMipCount = pHeader[VTF_OFFSET_MIP_COUNT];
	const int nDepth = ( nMinorVersion >= 2 ) ? Read16( pHeader + VTF_OFFSET_DEPTH ) : 1;
	const int nFaces = ( nFlags & TEXTUREFLAGS_ENVMAP ) ? 6 : 1;
	if ( nWidth <= 0 || nHeight <= 0 || nDepth <= 0 || nFrames <= 0 )
		return Fail( "invalid VTF dimensions" );

	// The KTX2 layout must describe exactly the VTF's subresources.
	if ( nPixelWidth != uint32( nWidth ) || nPixelHeight != uint32( nHeight ) ||
		 nPixelDepth != uint32( nDepth > 1 ? nDepth : 0 ) || nFaceCount != uint32( nFaces ) ||
		 nLayerCount != uint32( nFrames > 1 ? nFrames : 0 ) || nLevelCount != nMipCount )
	{
		return Fail( "KTX2 %ux%ux%u, %u layers, %u faces, %u levels does not match VTF %dx%dx%d, %d frames, %d faces, %u mips",
			nPixelWidth, nPixelHeight, nPixelDepth, nLayerCount, nFaceCount, nLevelCount,
			nWidth, nHeight, nDepth, nFrames, nFaces, nMipCount );
	}

	ImageFormat nFormat;
	if ( !MapImageFormat( nVkFormat, nSourceFormat, nDepth, &nFormat ) )
		return false;

	vtf.Purge();
	vtf.EnsureCapacity( m_nSize );
	vtf.Put( pHeader, nHeaderSize );
	Write32( (unsigned char *)vtf.Base() + VTF_OFFSET_IMAGE_FORMAT, nFormat );

	int nLowResBytes = 0;
	const int nLowResFormat = (int)Read32( pHeader + VTF_OFFSET_LOW_RES_FORMAT );
	const int nLowResWidth = pHeader[VTF_OFFSET_LOW_RES_WIDTH];
	const int nLowResHeight = pHeader[VTF_OFFSET_LOW_RES_HEIGHT];
	if ( nLowResWidth && nLowResHeight && nLowResFormat != IMAGE_FORMAT_UNKNOWN )
	{
		if ( nLowResFormat < 0 || nLowResFormat >= NUM_IMAGE_FORMATS )
			return Fail( "invalid low-res format %d", nLowResFormat );
		nLowResBytes = ImageLoader::GetMemRequired( nLowResWidth, nLowResHeight, 1, (ImageFormat)nLowResFormat, false );
	}

	// Resources go before the image so partial reads (mip skipping) keep them.
	uint32 nOffset;
	if ( nMinorVersion < 3 )
	{
		// Pre-7.3 files: header, low-res image, image data.
		if ( nLowResBytes && !PutResource( vtf, VTF_LEGACY_RSRC_LOW_RES_IMAGE, nLowResBytes, &nOffset ) )
			return false;
	}
	else
	{
		const uint32 nResources = Read32( pHeader + VTF_OFFSET_RESOURCE_COUNT );
		if ( nResources > VTF_MAX_RESOURCES || VTF_OFFSET_RESOURCES + nResources * 8 > nHeaderSize )
			return Fail( "invalid resource dictionary (%u entries)", nResources );

		int nImageEntry = -1;
		for ( uint32 i = 0; i < nResources; ++i )
		{
			const int nEntry = VTF_OFFSET_RESOURCES + i * 8;
			const uint32 nType = Read32( pHeader + nEntry );
			if ( nType & RSRCF_HAS_NO_DATA_CHUNK )
				continue;	// value lives in the dictionary

			if ( nType == VTF_LEGACY_RSRC_IMAGE )
			{
				if ( nImageEntry >= 0 )
					return Fail( "duplicate image resource" );
				nImageEntry = nEntry;
				continue;
			}

			const bool bLowRes = ( nType == VTF_LEGACY_RSRC_LOW_RES_IMAGE );
			if ( bLowRes && !nLowResBytes )
				return Fail( "low-res resource without a low-res image" );
			if ( !PutResource( vtf, nType, bLowRes ? nLowResBytes : -1, &nOffset ) )
				return false;
			Write32( (unsigned char *)vtf.Base() + nEntry + 4, nOffset );
		}
		if ( nImageEntry < 0 )
			return Fail( "missing image resource" );
		Write32( (unsigned char *)vtf.Base() + nImageEntry + 4, vtf.TellPut() );
	}

	// VTF image data: mips from smallest to largest, then frames, then faces.
	// KTX2 levels hold layers (frames), then faces, then depth slices.
	// Legacy cube map sphere faces are not stored; the loader accepts six faces.
	for ( int nMip = int( nLevelCount ) - 1; nMip >= 0; --nMip )
	{
		const unsigned char *pLevel = m_pData + KTX2_LEVEL_INDEX_OFFSET + nMip * 24;
		const uint64 nLevelOffset = Read64( pLevel );
		const uint64 nLevelLength = Read64( pLevel + 8 );
		const uint64 nUncompressedLength = Read64( pLevel + 16 );

		const int nMipWidth = MAX( 1, nWidth >> nMip );
		const int nMipHeight = MAX( 1, nHeight >> nMip );
		const int nMipDepth = MAX( 1, nDepth >> nMip );
		const int nImageSize = ImageLoader::GetMemRequired( nMipWidth, nMipHeight, nMipDepth, nFormat, false );
		const uint64 nExpected = uint64( nImageSize ) * nFrames * nFaces;
		if ( nLevelLength != nExpected || nUncompressedLength != nExpected || !RangeFits( nLevelOffset, nLevelLength, m_nSize ) )
		{
			return Fail( "level %d is %llu bytes at %llu, expected %llu", nMip,
				(unsigned long long)nLevelLength, (unsigned long long)nLevelOffset, (unsigned long long)nExpected );
		}
		vtf.Put( m_pData + nLevelOffset, int( nLevelLength ) );
	}

	if ( !vtf.IsValid() )
		return Fail( "out of memory" );
	return true;
}

} // namespace

bool ConvertKTX2ToVTF( const void *pKTX2, int nKTX2Size, CUtlBuffer &vtf, char *pError, int nErrorSize )
{
	CKTX2ToVTF converter( (const unsigned char *)pKTX2, nKTX2Size, pError, nErrorSize );
	return converter.Convert( vtf );
}
