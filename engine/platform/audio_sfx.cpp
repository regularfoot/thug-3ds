// Native sound-device layer beneath the WHOLE original CSfxManager. Resource
// keys, positional volume, event IDs, priorities and control IDs stay original.
#include <core/defines.h>
#include <gel/soundfx/soundfx.h>
#include <gel/music/music.h>
#include <source_port/audio.h>
#include <source_port/application_os.h>
#include <source_port/thread_os.h>
#include <sys/file/filesys.h>
#include <gel/soundfx/sdl/ima.h>
#include "audio_internal.h"
#include <algorithm>
#include <vector>
namespace {
constexpr std::size_t budget=24*1024*1024;
constexpr int OldEffectVoices = 12;
SourcePort::Voice voices[NUM_VOICES];
std::size_t bytes_loaded=0;
void controller() { Dbg_Assert(source_thread_is_controller()); }
class EffectStorageScope {
public:
    EffectStorageScope(){
        // Console SFX live in sound RAM; native PCM lives in the original
        // root region. Reuse its top-down PRE/level holes instead of growing
        // the permanent bottom-up frontier past already freed level storage.
        // Never inherit a temporary player's heap or escape the 64MiB arena.
        auto& memory=Mem::Manager::sHandle();memory.PushContext(memory.TopDownHeap());
    }
    ~EffectStorageScope(){Mem::Manager::sHandle().PopContext();}
};
uint32 u32(const unsigned char* p) { return uint32(p[0])|(uint32(p[1])<<8)|(uint32(p[2])<<16)|(uint32(p[3])<<24); }
bool wave(const char* name,std::vector<unsigned char>& raw,unsigned& rate,bool& loop,std::size_t& offset,std::size_t& size,bool& xbox_ima) {
    void* file=File::Open(name,"rb"); if (!file) return false;
    auto length=File::GetFileSize(file);
    bool ok=length>=44 && length<=8*1024*1024;
    if (ok) { raw.resize(length); ok=File::Read(raw.data(),1,raw.size(),file)==raw.size(); }
    File::Close(file); if (!ok || std::memcmp(raw.data(),"RIFF",4) || std::memcmp(raw.data()+8,"WAVE",4)) return false;
    bool format=false,data=false; loop=xbox_ima=false;
    for (std::size_t at=12;at+8<=raw.size();) {
        auto n=u32(raw.data()+at+4); const auto* tag=raw.data()+at;
        at+=8; if (n>raw.size()-at) return false;
        if (!std::memcmp(tag,"fmt ",4)) {
            if (format || n<16) return false;
            rate=u32(raw.data()+at+4); format=rate>=8000 && rate<=96000;
            xbox_ima=u32(raw.data()+at)==0x00010069 && u32(raw.data()+at+12)==0x00040024;
        } else if (!std::memcmp(tag,"data",4)) { if (data) return false; offset=at; size=n; data=true; }
        else if (!std::memcmp(tag,"smpl",4) && n>=32) loop=u32(raw.data()+at+28)!=0;
        at+=n;
        // The original loose writer omits odd RIFF padding; retail PRE WAVs
        // can include it. Accept only the actual bounded next chunk position.
        if ((n&1) && at<raw.size() && raw[at]==0) ++at;
    }
    return format && data && size;
}
}
namespace SourcePort {
std::shared_ptr<Sample> LoadEffect(const char* name) {
    controller(); if (!name || std::strlen(name)>100) return {};
    EffectStorageScope storage;
    char path[128]; std::snprintf(path,sizeof(path),"sounds/pcm/%s.pcm",name);
    bool archive=File::Exist(path);
    if (!archive) std::snprintf(path,sizeof(path),"sounds/%s.snd",name);
    std::vector<unsigned char> raw; unsigned rate=0; bool loop=false,xbox_ima=false; std::size_t offset=0,size=0;
    if (!wave(path,raw,rate,loop,offset,size,xbox_ima)) return {};
    auto sample=std::make_shared<Sample>(); sample->rate=rate; sample->looping=loop;
    if (archive) {
        if(raw.capacity()>budget-bytes_loaded){errno=ENOMEM;return {};}
        // Keep the exact original Xbox ADPCM WAV, not an entire expanded PCM
        // bank. Every active voice has its own bounded decoder and loop state.
        if (xbox_ima && size%36==0) {
            for (std::size_t at=offset;at<offset+size;at+=36)
                if (raw[at+2]>88 || raw[at+3]) return {};
            sample->ima_offset=offset;
            sample->ima_bytes=size;
        } else {
            MediaDecoder decoder;if(!decoder.OpenMemory(raw.data(),raw.size()))return {};
            sample->rate=decoder.Rate();
        }
        DiskResourceSlice slice;
        if (source_application_os_is_old_3ds() && FindDiskResourceSlice(path, slice))
            sample->disk=std::move(slice);
        else sample->encoded=std::move(raw);
    } else {
        // These PC loose SNDs are the original raw-nibble IMA stream, despite
        // stale PCM tags in their RIFF headers. Use the actual source decoder.
        if (size> (budget-bytes_loaded)/4) { errno=ENOMEM; return {}; }
        sample->mono.resize(size*2);
        IMA::IMA decoder(raw.data()+offset,size);
        for (auto& v:sample->mono) v=decoder.Fetch();
    }
    if (sample->mono.empty()&&sample->encoded.empty()&&!sample->disk) return {};
    return sample;
}
void MixEffects(float* output,std::size_t frames) {
    const auto gain = Sfx::gSfxVolume * .01f;
    const int count = source_application_os_is_old_3ds() ? OldEffectVoices : NUM_VOICES;
    for (int i=0;i<count;++i) {
        auto& voice=voices[i];
        voice.Mix(output,frames,gain);
        if (!voice.playing && voice.disk_bytes) {
            voice.stream.reset();
            voice.disk_bytes.reset();
        }
    }
}
}
namespace Sfx {
class CXBSound {
public:
    std::shared_ptr<SourcePort::Sample> sample;
    explicit CXBSound(std::shared_ptr<SourcePort::Sample> s):sample(std::move(s)) { bytes_loaded+=sample->Bytes(); }
    ~CXBSound() { bytes_loaded-=sample->Bytes(); }
};
struct Owned { PlatformWaveInfo* info; CXBSound* sound; };
static std::vector<Owned> owners;
float gSfxVolume=100;
void InitSoundFX(CSfxManager*) { SourcePort::OpenAudio(); }
bool LoadSoundPlease(const char* name,uint32,PlatformWaveInfo* info,bool permanent) {
    controller(); Dbg_Assert(info && !info->p_sound_data);
    EffectStorageScope storage;
    auto decoded=SourcePort::LoadEffect(name); if (!decoded) return false;
    auto* sound=new CXBSound(std::move(decoded));
    info->p_sound_data=sound; info->looping=sound->sample->looping; info->permanent=permanent;
    owners.push_back({info,sound}); return true;
}
int PlaySoundPlease(PlatformWaveInfo* info,sVolume* volume,float pitch) {
    controller(); if (!SourcePort::AudioReady() || !info || !info->p_sound_data || !std::isfinite(pitch) || pitch<=0 || pitch>400) return -1;
    const bool old_console = source_application_os_is_old_3ds();
    const int max_voices = old_console ? OldEffectVoices : NUM_VOICES;
    for (int i=0;i<max_voices;++i) if (!voices[i].playing) {
        auto& v=voices[i]; v.stream.reset(); v.disk_bytes.reset(); v.sample=info->p_sound_data->sample; v.rate=v.sample->rate; v.pitch=pitch;
        SourcePort::VolumePair(volume,v.left,v.right);
        v.paused=false; v.Begin(); return v.playing?i:-1;
    }
    return -1;
}
void StopSoundPlease(int which) {
    controller(); if (which<0 || which>=NUM_VOICES) return;
    voices[which].playing=voices[which].prepared=false;voices[which].stream.reset();voices[which].disk_bytes.reset();voices[which].sample.reset();
}
void StopAllSoundFX() {
    // Level scripts own the soundtrack and explicitly stop it at transitions.
    controller(); Pcm::StopStreams();
    for (int i=0;i<NUM_VOICES;++i) StopSoundPlease(i);
}
bool VoiceIsOn(int which) { controller(); return which>=0 && which<NUM_VOICES && voices[which].playing; }
void SetVoiceParameters(int which,sVolume* volume,float pitch) {
    controller(); if (which<0 || which>=NUM_VOICES || !voices[which].playing) return;
    if (pitch!=0) { Dbg_Assert(std::isfinite(pitch) && pitch>0 && pitch<=400); voices[which].pitch=pitch; }
    SourcePort::VolumePair(volume,voices[which].left,voices[which].right);
}
void PauseSoundsPlease() { controller(); for (auto& v:voices) v.left=v.right=0; }
void SetVolumePlease(float level) { controller(); Dbg_Assert(std::isfinite(level) && level>=0 && level<=100); gSfxVolume=level; }
void SetReverbPlease(float level,int mode,bool instant) { SourcePort::SetNativeReverb(level,mode,instant); }
void PerFrameUpdate() { if (SourcePort::AudioReady()) SourcePort::PumpAudio(); }
int GetMemAvailable() { controller(); return int(budget-bytes_loaded); }
void CleanUpSoundFX() {
    StopAllSoundFX(); SetReverbPlease(0);
    for (auto i=owners.begin();i!=owners.end();) {
        if (!i->info->permanent) { i->info->p_sound_data=nullptr; delete i->sound; i=owners.erase(i); }
        else ++i;
    }
    NumWavesInTable=0;
}
void MixSoundFX(char* buffer,size_t size) { controller(); SourcePort::MixEffects(reinterpret_cast<float*>(buffer),size/(sizeof(float)*2)); }
}
namespace SourcePort {
void ReleasePermanentAudio() {
    for (int i=0;i<NUM_VOICES;++i) Sfx::StopSoundPlease(i);
    for (auto& o:Sfx::owners) { o.info->p_sound_data=nullptr; delete o.sound; }
    std::vector<Sfx::Owned>().swap(Sfx::owners); Sfx::NumWavesInPermTable=Sfx::NumWavesInTable=0;
    Dbg_Assert(!bytes_loaded);
}
}
