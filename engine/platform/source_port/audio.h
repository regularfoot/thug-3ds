#pragma once
#include <source_port/audio_os.h>
#include <cstddef>
#include <cstdint>
namespace SourcePort {
struct AudioSink {
    void* owner;
    bool (*open)(void*);
    bool (*available)(void*);
    bool (*submit)(void*,const std::int16_t*,unsigned);
    void (*close)(void*);
};
void BindAudioSink(AudioSink sink);
void UnbindAudioSink(void* owner);
void OpenAudio();
void PumpAudio();
void CloseAudio();
bool AudioReady();
class AudioSession {
public:
    AudioSession(){OpenAudio();}
    ~AudioSession(){CloseAudio();}
    AudioSession(const AudioSession&)=delete;
    AudioSession& operator=(const AudioSession&)=delete;
};
struct AudioMixTicks {uint64_t effects,reverb,streams;unsigned blocks;};
AudioMixTicks CurrentAudioMixTicks();
void RenderAudio(float* stereo, std::size_t frames);
void ReleasePermanentAudio();
}
