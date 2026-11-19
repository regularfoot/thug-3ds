#pragma once
#include <cstddef>
#include <cstdint>
namespace SourcePort {
// Real Bink/WAV decoder, bounded to its original file/WAD entry. Output is
// stereo float at the source rate; hardware resampling belongs to the mixer.
class MediaDecoder {
public:
    MediaDecoder() = default;
    ~MediaDecoder();
    MediaDecoder(const MediaDecoder&) = delete;
    MediaDecoder& operator=(const MediaDecoder&) = delete;
    bool Open(const char* name, std::uint64_t offset = 0, std::uint64_t length = 0);
    // Borrowed immutable bytes must outlive this decoder (voice owns sample).
    bool OpenMemory(const std::uint8_t* bytes, std::size_t length);
    bool Rewind();
    void Close();
    std::size_t Read(float* stereo, std::size_t frames);
    unsigned Rate() const;
    bool Ended() const;
    bool Failed() const;
private:
    struct State;
    State* state_ = nullptr;
    bool failed_ = false;
};
}
