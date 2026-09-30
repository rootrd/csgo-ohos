#include <SDL3/SDL.h>
#include "tier0/platform.h"
#include "tier0/dbg.h"
#include "ivoicerecord.h"

namespace {
class VoiceRecordSDL final : public IVoiceRecord
{
    SDL_AudioStream *m_stream = nullptr;
    int m_sampleRate;
    bool m_audioInitialized = false;
public:
    explicit VoiceRecordSDL(int sampleRate) : m_sampleRate(sampleRate) {}
    ~VoiceRecordSDL() override
    {
        if (m_stream) SDL_DestroyAudioStream(m_stream);
        if (m_audioInitialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    void Release() override { delete this; }
    bool RecordStart() override
    {
        // Opening a recording device asks for microphone permission through
        // SDLActivity. Do this only when recording is requested, not on startup.
        if (!m_audioInitialized)
        {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
            m_audioInitialized = true;
        }
        if (!m_stream)
        {
            const SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, m_sampleRate};
            m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
            if (!m_stream)
            {
                Warning("SDL3 microphone unavailable: %s\n", SDL_GetError());
                return false;
            }
        }
        SDL_ClearAudioStream(m_stream);
        return SDL_ResumeAudioStreamDevice(m_stream);
    }
    void RecordStop() override
    {
        if (!m_stream) return;
        SDL_PauseAudioStreamDevice(m_stream);
        SDL_ClearAudioStream(m_stream);
    }
    void Idle() override {}
    int GetRecordedData(short *out, int samplesWanted) override
    {
        if (!m_stream || !out || samplesWanted <= 0) return 0;
        const int wanted = samplesWanted * int(sizeof(short));
        int available = SDL_GetAudioStreamAvailable(m_stream);
        if (available <= 0) return 0;
        short discard[512];
        while (available > wanted)
        {
            const int bytes = (available - wanted < int(sizeof(discard))) ? available - wanted : int(sizeof(discard));
            const int read = SDL_GetAudioStreamData(m_stream, discard, bytes);
            if (read <= 0) return 0;
            available -= read;
        }
        const int read = SDL_GetAudioStreamData(m_stream, out, wanted);
        return read > 0 ? read / int(sizeof(short)) : 0;
    }
};
}

IVoiceRecord *CreateVoiceRecord_DSound(int sampleRate)
{
    return sampleRate > 0 ? new VoiceRecordSDL(sampleRate) : nullptr;
}
