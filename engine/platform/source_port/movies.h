#pragma once
#include <cstdint>
#include <vector>

namespace SourcePort {
class MovieVideo {
public:
    static constexpr unsigned Width=320,Height=240,TextureWidth=512,TextureHeight=256;
    MovieVideo()=default;
    ~MovieVideo();
    MovieVideo(const MovieVideo&)=delete;
    MovieVideo& operator=(const MovieVideo&)=delete;
    bool Open(const char* path);
    // One decoded picture, end of stream, or a negative decode error.
    int Read();
    void Tile(std::vector<std::uint8_t>& pixels) const;
    std::uint64_t FrameTicks(std::uint64_t clock_hz) const;
private:
    struct State;
    State* state_=nullptr;
};
void PlayNativeMovie(const char* name);
void PlayOpeningMovies();
}
