//========= Copyright (c) Valve Corporation, All rights reserved. ============//
//
// Purpose: IVAudio for platforms without the Miles Sound System (Linux and
//          Android): MP3 stream decoding with minimp3.
//
//=============================================================================//

// minimp3 comes first so that no engine macro can leak into its implementation.
#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"

#include <string.h>

#include "tier1/interface.h"
#include "vaudio/ivaudio.h"
#include "tier0/dbg.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// minimp3 accepts a frame only when the following frame header is buffered too,
// and it drops the bit reservoir whenever it has to resynchronize. Keeping
// several maximum-size frames (2881 bytes) buffered avoids both mid-stream.
enum
{
	MP3_INPUT_BUFFER_SIZE = 16384,
	MP3_REFILL_THRESHOLD = 8192,
};

class CMiniMP3 final : public IAudioStream
{
public:
	explicit CMiniMP3( IAudioStreamEvent *pEventHandler ) : m_pEventHandler( pEventHandler )
	{
		mp3dec_init( &m_Decoder );
	}

	// Decodes the first audio frame, which defines the output format.
	bool Init() { return DecodeFrame(); }

	// IAudioStream functions
	int Decode( void *pBuffer, unsigned int bufferSize ) override;
	int GetOutputBits() override { return 16; }
	int GetOutputRate() override { return m_nRate; }
	int GetOutputChannels() override { return m_nChannels; }
	// Offset of the next undecoded frame, in the event handler's offset space.
	unsigned int GetPosition() override { return m_nInputPosition; }
	void SetPosition( unsigned int position ) override;

private:
	void Refill();
	bool DecodeFrame();

	IAudioStreamEvent *m_pEventHandler;
	mp3dec_t m_Decoder;
	int m_nRate = 0;
	int m_nChannels = 0;

	unsigned char m_Input[MP3_INPUT_BUFFER_SIZE];
	int m_nInputStart = 0;
	int m_nInputEnd = 0;
	unsigned int m_nInputPosition = 0;
	// The first request must be for offset 0: that is where the mixer skips ID3v2 tags.
	int m_nRequestOffset = 0;
	bool m_bEndOfStream = false;

	mp3d_sample_t m_Pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	int m_nPcmCount = 0;	// interleaved samples in m_Pcm
	int m_nPcmRead = 0;
};

void CMiniMP3::Refill()
{
	memmove( m_Input, m_Input + m_nInputStart, m_nInputEnd - m_nInputStart );
	m_nInputEnd -= m_nInputStart;
	m_nInputStart = 0;
	while ( !m_bEndOfStream && m_nInputEnd < MP3_INPUT_BUFFER_SIZE )
	{
		// Offset -1 continues where the previous request ended; no data means end of stream.
		const int nRead = m_pEventHandler->StreamRequestData( m_Input + m_nInputEnd, MP3_INPUT_BUFFER_SIZE - m_nInputEnd, m_nRequestOffset );
		m_nRequestOffset = -1;
		if ( nRead > 0 )
			m_nInputEnd += nRead;
		else
			m_bEndOfStream = true;
	}
}

bool CMiniMP3::DecodeFrame()
{
	for ( ;; )
	{
		if ( m_nInputEnd - m_nInputStart < MP3_REFILL_THRESHOLD )
			Refill();
		const int nAvailable = m_nInputEnd - m_nInputStart;
		if ( !nAvailable )
			return false;

		mp3dec_frame_info_t info;
		const int nSamples = mp3dec_decode_frame( &m_Decoder, m_Input + m_nInputStart, nAvailable, m_Pcm, &info );
		m_nInputStart += info.frame_bytes;
		m_nInputPosition += info.frame_bytes;
		if ( nSamples )
		{
			if ( !m_nChannels )
			{
				m_nChannels = info.channels;
				m_nRate = info.hz;
			}
			if ( info.hz != m_nRate )
			{
				Warning( "MP3 stream changes sample rate from %d to %d Hz; playback stops there.\n", m_nRate, info.hz );
				m_nInputStart = m_nInputEnd;
				m_bEndOfStream = true;
				return false;
			}
			// MPEG allows the channel mode to change per frame; the mixer keeps the first one.
			if ( info.channels < m_nChannels )
			{
				for ( int i = nSamples - 1; i >= 0; --i )
					m_Pcm[2 * i] = m_Pcm[2 * i + 1] = m_Pcm[i];
			}
			else if ( info.channels > m_nChannels )
			{
				for ( int i = 0; i < nSamples; ++i )
					m_Pcm[i] = mp3d_sample_t( ( m_Pcm[2 * i] + m_Pcm[2 * i + 1] ) / 2 );
			}
			m_nPcmCount = nSamples * m_nChannels;
			m_nPcmRead = 0;
			return true;
		}
		// No samples: skipped data (tags, junk, a frame waiting for its bit reservoir)
		// or an incomplete frame. With several frames buffered, an incomplete frame in
		// a full buffer or at the end of the stream is truncated data.
		if ( !info.frame_bytes )
		{
			if ( m_bEndOfStream || nAvailable == MP3_INPUT_BUFFER_SIZE )
				return false;
			Refill();
		}
	}
}

int CMiniMP3::Decode( void *pBuffer, unsigned int bufferSize )
{
	mp3d_sample_t *pOutput = static_cast<mp3d_sample_t *>( pBuffer );
	const int nWanted = int( bufferSize / ( sizeof( mp3d_sample_t ) * m_nChannels ) ) * m_nChannels;
	int nWritten = 0;
	while ( nWritten < nWanted && ( m_nPcmRead < m_nPcmCount || DecodeFrame() ) )
	{
		const int nPending = m_nPcmCount - m_nPcmRead;
		const int nCopy = nPending < nWanted - nWritten ? nPending : nWanted - nWritten;
		memcpy( pOutput + nWritten, m_Pcm + m_nPcmRead, nCopy * sizeof( mp3d_sample_t ) );
		m_nPcmRead += nCopy;
		nWritten += nCopy;
	}
	return nWritten * int( sizeof( mp3d_sample_t ) );
}

void CMiniMP3::SetPosition( unsigned int position )
{
	// Resume at an offset from GetPosition(); minimp3 resynchronizes on the next frame header.
	mp3dec_init( &m_Decoder );
	m_nInputStart = m_nInputEnd = 0;
	m_nInputPosition = position;
	m_nRequestOffset = int( position );
	m_bEndOfStream = false;
	m_nPcmCount = m_nPcmRead = 0;
}

class CVAudio final : public IVAudio
{
public:
	IAudioStream *CreateMP3StreamDecoder( IAudioStreamEvent *pEventHandler ) override
	{
		CMiniMP3 *pDecoder = new CMiniMP3( pEventHandler );
		if ( !pDecoder->Init() )
		{
			delete pDecoder;
			return NULL;
		}
		return pDecoder;
	}

	void DestroyMP3StreamDecoder( IAudioStream *pDecoder ) override
	{
		delete pDecoder;
	}

	// Only Miles can back the engine's Bink and surround audio device; none exists here.
	void *CreateMilesAudioEngine() override { return NULL; }
	void DestroyMilesAudioEngine( void *pEngine ) override { Assert( !pEngine ); }
};

// VAudioInit transfers ownership to the engine, which deletes IVAudio before
// unloading the codec module in S_Shutdown.
EXPOSE_INTERFACE( CVAudio, IVAudio, VAUDIO_INTERFACE_VERSION );
