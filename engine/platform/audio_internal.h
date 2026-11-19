#pragma once
#include <source_port/media.h>
#include <source_port/files.h>
#include <optional>
#include <gel/soundfx/soundfx.h>
#include <memory>
#include <vector>
namespace SourcePort {
struct Sample {
    std::vector<std::int16_t> mono;
    std::vector<std::uint8_t> encoded;
    std::optional<DiskResourceSlice> disk;
    std::weak_ptr<const std::vector<std::uint8_t>> active_bytes;
    // Validated mono Xbox IMA-WAV data, in independently decodable 36-byte blocks.
    std::size_t ima_offset=0,ima_bytes=0;
    unsigned rate=0;bool looping=false;
    std::size_t Bytes()const{return mono.capacity()*sizeof(std::int16_t)+encoded.capacity();}
};
struct Voice {
    std::shared_ptr<Sample> sample;
    std::shared_ptr<const std::vector<std::uint8_t>> disk_bytes;
    std::unique_ptr<MediaDecoder> stream;
    float block[2048]{};
    std::size_t block_at=0,block_frames=0;
    float a[2]{},b[2]{};
    double phase=0;
    float left=1,right=1,pitch=100;
    bool prepared=false,playing=false,paused=false,ended=false,starved=false;
    int file_slot=-1;
    bool primed=false;
    unsigned rate=0;
    std::size_t sample_at=0;
    bool Next(float* frame);
    void Begin();
    void Mix(float* stereo,std::size_t frames,float gain=1);
};
bool DecodeEffectBlock(const uint8* bytes,float* output,bool lookup);
void MixEffects(float* stereo,std::size_t frames);
void QuantizeAudioBlock(const float*,std::int16_t*,unsigned);
std::int16_t QuantizeAudioSample(float sample);
void ResetAudioTiming();
void ReportAudioTiming(unsigned frame);
void MixStreams(float* stereo,std::size_t frames);
void StopNativeStreams();
void ResetNativeStreamCatalogs();
void SetNativeReverb(float level,int mode,bool instant);
std::shared_ptr<Sample> LoadEffect(const char* name);
void VolumePair(Sfx::sVolume* volume,float& left,float& right);
}
