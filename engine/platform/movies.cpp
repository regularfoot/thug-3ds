#include <core/defines.h>
#include <source_port/movies.h>
#ifdef __3DS__
#include <3ds.h>
#include <source_port/application.h>
#include <source_port/diagnostics.h>
#include <cerrno>
#include <source_port/audio_os.h>
#include <source_port/clock_os.h>
#include <source_port/files.h>
#include <source_port/graphics.h>
#include <source_port/launch_paths.h>
#include <source_port/thread_os.h>
#include "audio_internal.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace {
enum class End {Complete,Skip,SkipOpening,Quit,Failed};
bool playing=false;

void fill_audio(SourcePort::Voice& voice){
    for(unsigned i=0;i<SOURCE_AUDIO_BUFFERS&&voice.playing&&source_audio_available()>0;++i){
        float mix[SOURCE_AUDIO_FRAMES*2]{};
        int16_t pcm[SOURCE_AUDIO_FRAMES*2];
        voice.Mix(mix,SOURCE_AUDIO_FRAMES);
        for(unsigned j=0;j<SOURCE_AUDIO_FRAMES*2;++j)
            pcm[j]=int16_t(std::lround(std::clamp(mix[j],-1.f,1.f)*32767.f));
        if(source_audio_submit(pcm,SOURCE_AUDIO_FRAMES)!=0)break;
    }
}

End movie(const char* request){
    if(!request||playing)return End::Failed;
    std::string name=request;
    std::replace(name.begin(),name.end(),'\\','/');
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(name.rfind("movies/bik/",0)==0)name.erase(0,11);
    else if(name.rfind("movies/",0)==0)name.erase(0,7);
    if(name.size()>=4&&name.compare(name.size()-4,4,".bik")==0)name.resize(name.size()-4);
    if(name.empty()||name.size()>32||name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos)
        return End::Failed;
    const auto path="movies/bik/"+name+".bik";
    char resolved[1024];
    SourcePort::MovieVideo video;
    if(!SourcePort::ResolveResourcePath(path.c_str(),resolved,sizeof(resolved))){
        SourcePort::Log("MOVIE_OPEN_ERROR path=%s stage=path errno=%d",path.c_str(),errno);
        return End::Failed;
    }
    if(!video.Open(resolved))
        return End::Failed;

    SourcePort::Voice voice;
    voice.stream=std::make_unique<SourcePort::MediaDecoder>();
    if(voice.stream->Open(path.c_str())){
        voice.rate=voice.stream->Rate();
        source_audio_discard();
        voice.Begin();
    }else voice.stream.reset();

    using Video=SourcePort::MovieVideo;
    constexpr float u0=.5f/Video::TextureWidth,u1=(Video::Width-.5f)/Video::TextureWidth;
    constexpr float v0=1-.5f/Video::TextureHeight,v1=1-(Video::Height-.5f)/Video::TextureHeight;
    // Preserve the source 4:3 aspect on the 400x240 display.
    constexpr SourceRenderVertex a{64,0,0,255,255,255,255,u0,v0};
    constexpr SourceRenderVertex b{64,448,0,255,255,255,255,u0,v1};
    constexpr SourceRenderVertex c{576,448,0,255,255,255,255,u1,v1};
    constexpr SourceRenderVertex d{576,0,0,255,255,255,255,u1,v0};
    const SourceRenderVertex quad[]={a,b,c,a,c,d};
    auto* mesh=source_graphics_mesh_create(quad,6);
    if(!mesh){
        SourcePort::Log("MOVIE_OPEN_ERROR path=%s stage=mesh errno=%d",path.c_str(),errno);
        return End::Failed;
    }

    SourceDrawPacket packet{};
    packet.mesh=mesh;
    packet.count=6;
    packet.width=400;
    packet.height=240;
    packet.orthographic=1;
    for(unsigned i=0;i<4;++i)packet.model_view[5*i]=1;
    std::vector<uint8_t> tiled;
    const auto hz=source_clock_frequency(),interval=video.FrameTicks(hz);
    std::uint64_t due=0,last=0;
    unsigned decoded=0,shown=0;
    End end=End::Complete;
    playing=true;
    auto poll=[&]{
        if(!SourcePort::PumpApplication())return End::Quit;
        hidScanInput();
        const auto pressed=hidKeysDown();
        if(pressed&KEY_START)return End::SkipOpening;
        if(pressed&(KEY_A|KEY_B))return End::Skip;
        fill_audio(voice);
        return End::Complete;
    };
    SourcePort::Log("MOVIE_BEGIN name=%s audio=%u",name.c_str(),voice.rate);
    SourcePort::CheckpointDiagnosticLog();
    for(;;){
        end=poll();
        if(end!=End::Complete)break;
        const int read=video.Read();
        if(read<=0){if(read<0)end=End::Failed;break;}
        ++decoded;
        auto now=source_clock_now();
        if(!due)due=now;
        while(now<due){
            end=poll();
            if(end!=End::Complete)break;
            svcSleepThread(1000000);
            now=source_clock_now();
        }
        if(end!=End::Complete)break;
        if(now<=due+interval||now-last>=hz/10){
            video.Tile(tiled);
            if(!packet.texture)packet.texture=source_graphics_texture_create(Video::TextureWidth,Video::TextureHeight,tiled.data(),tiled.size());
            else if(source_graphics_texture_update(packet.texture,tiled.data(),tiled.size())!=0){end=End::Failed;break;}
            if(!packet.texture||source_graphics_submit(&packet,1,0x000000ff,0)!=0){end=End::Failed;break;}
            last=now;
            ++shown;
        }
        due+=interval;
    }
    if(end==End::Complete){
        while(source_clock_now()<due&&end==End::Complete){
            end=poll();
            svcSleepThread(1000000);
        }
    }
    source_audio_discard();
    if(packet.texture)Dbg_Assert(source_graphics_texture_delete(packet.texture)==0);
    Dbg_Assert(source_graphics_mesh_delete(mesh)==0);
    playing=false;
    SourcePort::Log("MOVIE_END name=%s decoded=%u shown=%u result=%u",name.c_str(),decoded,shown,unsigned(end));
    SourcePort::CheckpointDiagnosticLog();
    return end;
}
}
#endif

namespace SourcePort {
void PlayNativeMovie(const char* name){
#ifdef __3DS__
    Dbg_Assert(source_thread_is_controller());
    if(movie(name)==End::Failed)Log("MOVIE_UNAVAILABLE name=%s",name?name:"");
#else
    (void)name;
#endif
}
void PlayOpeningMovies(){
#ifdef __3DS__
    static bool played=false;
    if(played)return;
    played=true;
    if (SkipOpeningMovies()) {
        Log("OPENING_MOVIES skipped by skip_opening.txt");
        return;
    }
    for(const char* name:{"atvi","nslogo","intro"}){
        auto end=movie(name);
        if(end==End::Failed)Log("MOVIE_UNAVAILABLE name=%s",name);
        if(end==End::SkipOpening||end==End::Quit)break;
    }
#endif
}
}
