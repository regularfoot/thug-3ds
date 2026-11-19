#pragma once
#include <cstdint>
#include <gel/music/sdl/p_music.h>

namespace SourcePort {
constexpr unsigned audio_file_slots = NUM_STREAMS + 1;
enum class AudioFileState { Idle, Pending, Opening, Ready, Ended, Failed };
struct AudioFileStatus {
    AudioFileState state;
    unsigned rate;
    unsigned blocks;
};

bool StartAudioFileWorker();
void StopAudioFileWorker();
bool QueueAudioFile(unsigned slot, const char* path, std::uint64_t offset, std::uint64_t length);
void CancelAudioFile(unsigned slot);
AudioFileStatus GetAudioFileStatus(unsigned slot);
// The caller supplies room for 1024 interleaved stereo frames.
unsigned ReadAudioFileBlock(unsigned slot, float* output);
}
