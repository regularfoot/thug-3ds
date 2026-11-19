#include <core/defines.h>
#include <gel/music/sdl/p_music.h>
#include <source_port/audio.h>
#include <source_port/audio_os.h>
#include <source_port/diagnostics.h>
#include <source_port/files.h>
#include <source_port/thread_os.h>
#include <sys/file/filesys.h>
#include "audio_internal.h"
#include "audio_file_stream.h"
#include <map>
#include <string>
#include <dirent.h>
#include <algorithm>
namespace {
struct Entry { std::string file; std::uint64_t offset=0,length=0; };
std::map<uint32,Entry> songs,streams;
std::string wad_file;
SourcePort::Voice music,voices[NUM_STREAMS];
std::uint64_t music_missing_frames=0, music_gap_frames=0, music_output_frames=0;
unsigned music_report_frames=0;
bool indexed=false;
float music_volume=1;
void controller() { Dbg_Assert(source_thread_is_controller()); }
bool slot(int which) { if (which>=0 && which<NUM_STREAMS) return true; errno=EINVAL; return false; }
uint32 music_key(const char* name){
    if(!name)return 0;const char* base=name;
    // Original PC p_music.cpp hashes the basename of PS2-style QB music paths.
    // Keep that wire-name contract for existence checks as well as playback.
    for(const char* p=name;*p;++p)if(*p=='\\'||*p=='/')base=p+1;
    return *base?Crc::GenerateCRCFromString(base):0;
}
void clear(SourcePort::Voice& v) {
    if (&v == &music) {
        if (v.primed)
            SourcePort::Log("MUSIC_BUFFER missing_frames=%llu output_rate=%u",
                static_cast<unsigned long long>(music_missing_frames),source_audio_rate());
        music_missing_frames=music_gap_frames=music_output_frames=0;
        music_report_frames=0;
    }
    if (v.file_slot>=0) SourcePort::CancelAudioFile(v.file_slot);
    v.file_slot=-1;
    v.prepared=v.playing=v.paused=v.ended=v.primed=v.starved=false;
    v.sample_at=v.block_at=v.block_frames=0;
}
bool prepare(SourcePort::Voice& v,unsigned slot,const Entry& entry) {
    clear(v);
    if (!SourcePort::QueueAudioFile(slot,entry.file.empty()?wad_file.c_str():entry.file.c_str(),entry.offset,entry.length)) return false;
    v.file_slot=slot;
    v.left=v.right=1;
    v.pitch=100;
    v.prepared=true;
    return true;
}
bool ready(const SourcePort::Voice& v) {
    if (!v.prepared) return false;
    const auto status=SourcePort::GetAudioFileStatus(v.file_slot);
    return (status.state==SourcePort::AudioFileState::Ready && status.blocks>=2) ||
        (status.state==SourcePort::AudioFileState::Ended && status.blocks);
}
bool start(SourcePort::Voice& v) {
    if (!SourcePort::AudioReady() || !v.prepared ||
        SourcePort::GetAudioFileStatus(v.file_slot).state==SourcePort::AudioFileState::Failed) return false;
    v.playing=true;
    v.paused=false;
    return true;
}
void index() {
    controller(); if (indexed) return;
    char directory[1024];
    if (SourcePort::ResolveResourcePath("streams/music",directory,sizeof(directory))) {
        DIR* dir=opendir(directory);
        if (dir) {
            while (auto* e=readdir(dir)) {
                std::string name(e->d_name); if (name.size()!=12 || strcasecmp(name.c_str()+8,".bik")) continue;
                char* end=nullptr; auto key=std::strtoul(name.c_str(),&end,16);
                if (end!=name.c_str()+8 || key>UINT32_MAX) continue;
                std::string relative="streams/music/"+name;
                void* f=File::Open(relative.c_str(),"rb"); if (!f) continue;
                unsigned char magic[4]{}; bool valid=File::GetFileSize(f)>=44 && File::Read(magic,1,4,f)==4 && !std::memcmp(magic,"BIK",3);
                File::Close(f); if (valid) songs.emplace(uint32(key),Entry{std::string(directory)+"/"+name,0,0});
            }
            closedir(dir);
        }
    }
    void* dat=File::Open("streams/pcm/pcm.dat","rb");
    void* wad=File::Open("streams/pcm/pcm.wad","rb");
    char wad_path[1024];
    if (dat && wad && SourcePort::ResolveResourcePath("streams/pcm/pcm.wad",wad_path,sizeof(wad_path))) {
        wad_file=wad_path;
        uint32 count=0; auto size=File::GetFileSize(dat),wad_size=File::GetFileSize(wad);
        bool valid=size>=4 && File::ReadInt(&count,dat)==4 && count<=100000 && std::uint64_t(size)==4+std::uint64_t(count)*12;
        for (uint32 i=0;valid && i<count;++i) {
            uint32 record[3]; valid=File::Read(record,4,3,dat)==12;
            if (!valid) break;
            if (wad_size<=0 || !record[2] || record[1]>std::uint64_t(wad_size) || record[2]>std::uint64_t(wad_size)-record[1]) { valid=false; break; }
            auto inserted=streams.emplace(record[0],Entry{{},record[1],record[2]});
            if (!inserted.second) { valid=false; break; }
        }
        if (!valid) { std::map<uint32,Entry>().swap(streams); errno=EILSEQ; }
    }
    if (dat) File::Close(dat); if (wad) File::Close(wad);
    indexed=true;
}
}
namespace SourcePort {
void MixStreams(float* output,std::size_t frames) {
    music.left=music.right=music_volume;
    const auto missing=music.Mix(output,frames);
    music_missing_frames+=missing;
    // A partially mixed block includes real music before a new gap. A paused
    // voice cannot establish that playback has resumed.
    if (music.playing && !music.paused && missing<frames && music_gap_frames) {
        const auto rate=source_audio_rate();
        if (music_gap_frames>=rate/4)
            Log("MUSIC_BUFFER resumed_after_ms=%llu",static_cast<unsigned long long>(music_gap_frames*1000/rate));
        music_gap_frames=0;
    }
    music_gap_frames+=missing;
    if (music.primed && music.playing && !music.paused) {
        music_output_frames+=frames;
        music_report_frames+=frames;
        const auto rate=source_audio_rate();
        if (music_report_frames>=12*rate) {
            Log("MUSIC_BUFFER output_frames=%llu missing_frames=%llu output_rate=%u",
                static_cast<unsigned long long>(music_output_frames),
                static_cast<unsigned long long>(music_missing_frames),rate);
            music_report_frames%=12*rate;
        }
    }
    for (auto& voice:voices) voice.Mix(output,frames,Sfx::gSfxVolume*0.01f);
}
void StopNativeStreams() { controller(); clear(music); for (auto& v:voices) clear(v); }
void ResetNativeStreamCatalogs() { controller(); std::map<uint32,Entry>().swap(songs); std::map<uint32,Entry>().swap(streams); std::string().swap(wad_file); indexed=false; music_volume=1; }
}
namespace Pcm {
void PCMAudio_Init() { SourcePort::OpenAudio(); index(); }
int PCMAudio_Update() { if (SourcePort::AudioReady()) SourcePort::PumpAudio(); return 0; }
void PCMAudio_Mix(char* buffer,size_t size) { controller(); SourcePort::MixStreams(reinterpret_cast<float*>(buffer),size/(sizeof(float)*2)); }
bool PCMAudio_PreLoadMusicStream(uint32 key) {
    controller(); index(); auto it=songs.find(key); if (it==songs.end()) { clear(music);return false; } return prepare(music,0,it->second);
}
bool PCMAudio_PreLoadMusicStreamDone() { controller(); return ready(music); }
bool PCMAudio_StartPreLoadedMusicStream() {
    controller(); return start(music);
}
bool PCMAudio_PlayMusicTrack(uint32 key) { return PCMAudio_PreLoadMusicStream(key) && PCMAudio_StartPreLoadedMusicStream(); }
bool PCMAudio_PlayMusicTrack(const char* name) { controller(); return name && *name && PCMAudio_PlayMusicTrack(music_key(name)); }
bool PCMAudio_PreLoadStream(uint32 key,int which) {
    controller(); if (!slot(which)) return false; index(); auto it=streams.find(key);
    if (it==streams.end()) { clear(voices[which]);return false; } return prepare(voices[which],which+1,it->second);
}
bool PCMAudio_PreLoadStreamDone(int which) { controller(); return slot(which) && ready(voices[which]); }
bool PCMAudio_SetStreamVolume(Sfx::sVolume* volume,int which) {
    controller(); if (!slot(which) || !voices[which].prepared || !volume) return false;
    SourcePort::VolumePair(volume,voices[which].left,voices[which].right); return true;
}
bool PCMAudio_SetStreamPitch(float pitch,int which) {
    controller(); if (!slot(which) || !voices[which].prepared || !std::isfinite(pitch) || pitch<=0 || pitch>400) return false;
    voices[which].pitch=pitch; return true;
}
bool PCMAudio_StartPreLoadedStream(int which,Sfx::sVolume* volume,float pitch) {
    controller(); if (!PCMAudio_SetStreamPitch(pitch,which)) return false;
    if (volume && !PCMAudio_SetStreamVolume(volume,which)) return false;
    return start(voices[which]);
}
bool PCMAudio_PlayStream(uint32 key,int which,Sfx::sVolume* volume,float pitch,bool preload) {
    if (!PCMAudio_PreLoadStream(key,which)) return false;
    return preload || PCMAudio_StartPreLoadedStream(which,volume,pitch);
}
bool PCMAudio_StartStream(int which) {
    controller(); return slot(which) && start(voices[which]);
}
void PCMAudio_StopMusic(bool) {
    controller();
    if (music.playing) SourcePort::Log("MUSIC_STOP requested=1");
    clear(music);
}
void PCMAudio_StopStream(int which,bool) { controller(); if (slot(which)) clear(voices[which]); }
void PCMAudio_StopStreams() { controller(); for (auto& v:voices) clear(v); }
int PCMAudio_GetMusicStatus() { controller(); return music.playing ? PCM_STATUS_PLAYING : PCM_STATUS_FREE; }
int PCMAudio_GetStreamStatus(int which) { controller(); return slot(which) && voices[which].playing ? PCM_STATUS_PLAYING : PCM_STATUS_FREE; }
void PCMAudio_Pause(bool pause,int channel) {
    controller(); if (channel==MUSIC_CHANNEL) music.paused=pause;
    else for (auto& v:voices) v.paused=pause;
}
int PCMAudio_SetMusicVolume(float volume) {
    controller(); if (!std::isfinite(volume) || volume<0 || volume>100) return -1;
    music_volume=volume*0.01f; return 0;
}
bool PCMAudio_TrackExists(const char* name,int channel) {
    controller(); if (!name||!*name) return false; index(); auto key=channel==MUSIC_CHANNEL?music_key(name):Crc::GenerateCRCFromString(name);
    return (channel==MUSIC_CHANNEL?songs:streams).count(key)!=0;
}
bool PCMAudio_LoadMusicHeader(const char* name) { controller(); if (!name || !*name) return false; index(); return !songs.empty(); }
bool PCMAudio_LoadStreamHeader(const char* name) { controller(); if (!name || !*name) return false; index(); return !streams.empty(); }
uint32 PCMAudio_FindNameFromChecksum(uint32 key,int channel) { controller(); index(); return (channel==MUSIC_CHANNEL?songs:streams).count(key)?key:0; }
}
