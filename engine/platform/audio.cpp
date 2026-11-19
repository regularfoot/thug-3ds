#include <limits>
#include <cstring>
#include <core/defines.h>
#include <source_port/audio.h>
#include <source_port/mixer_reverb.h>
#include <source_port/launch_paths.h>
#include <source_port/input.h>
#include <source_port/application_os.h>
#include <source_port/thread_os.h>
#include <source_port/memory.h>
#include <source_port/logic_timing.h>
#include <source_port/timer_state.h>
#include <sys/file/filesys.h>
#include "audio_internal.h"
#include "audio_file_stream.h"
#include <array>
#include <algorithm>
namespace {
SourcePort::AudioSink sink{};
bool opened=false;
struct AudioTiming {
    uint64 mix=0,quantize=0,submit=0,fx=0,wet=0,streams=0;
    unsigned blocks=0,max_refill=0;
} audio_timing;
// Main thread owns all game/mixer objects. NDSP only sees copied linear PCM.
SourcePort::Reverb reverb;
void controller() { Dbg_Assert(source_thread_is_controller()); }
constexpr std::array<int,89> ima_step{
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
    34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,
    598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,
    1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,
    5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,
    13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
constexpr std::array<int,8> ima_index{-1,-1,-1,-1,2,4,6,8};
// A magnitude needs 16 bits at index 88. The upper byte stores the next
// index so that each decoded nibble costs one lookup on ARM11.
constexpr auto ima_transitions=[]{
    std::array<std::array<uint32,8>,89> table{};
    for(int index=0;index<89;++index){
        for(int m=0;m<8;++m){
            const auto delta=(ima_step[index]*(2*m+1))>>3;
            const auto next=std::clamp(index+ima_index[m],0,88);
            table[index][m]=uint32(delta)|(uint32(next)<<16);
        }
    }
    return table;
}();
template<bool lookup>
bool decode_xbox_ima(const uint8* bytes,float* output) {
    int predictor=bytes[0]|int(bytes[1])<<8;
    if (predictor>=32768) predictor-=65536;
    int index=bytes[2];
    if (index>88 || bytes[3]) return false;
    const auto emit=[&](unsigned frame) {
        // At 16-bit input precision, rounding (p * 32767/32768) to
        // binary32 then to the nearest integer changes p by at most one.
        // The last rounded half-integer is 16415.5 at p=16416.
        const int pcm=predictor-(predictor>16416)+(predictor< -16416);
        output[frame*2]=output[frame*2+1]=float(pcm)/32768.f;
    };
    emit(0);
    for (unsigned i=0;i<64;++i) {
        const unsigned code=(bytes[4+i/2]>>(4*(i&1)))&15, magnitude=code&7;
        int delta;
        if constexpr(lookup){
            const auto transition=ima_transitions[index][magnitude];
            index=int(transition>>16);
            delta=int(transition&0xffff);
        }else{
            delta=(ima_step[index]*int(2*magnitude+1))>>3;
            index=std::clamp(index+ima_index[magnitude],0,88);
        }
        predictor=std::clamp(predictor+((code&8)?-delta:delta),-32768,32767);
        emit(i+1);
    }
    return true;
}
}
namespace SourcePort {
bool DecodeEffectBlock(const uint8* bytes,float* output,bool lookup){
    return lookup?decode_xbox_ima<true>(bytes,output):decode_xbox_ima<false>(bytes,output);
}
// Truncate in hardware, then apply lroundf's exact ties-away-from-zero
// rule. Adding .5 before truncating can round just-below-half values up.
std::int16_t QuantizeAudioSample(float sample) {
    if(std::isnan(sample))return 0;
    const float scaled=std::clamp(sample,-1.f,1.f)*32767.f;
    int whole=static_cast<int>(scaled);
    const float fraction=scaled-float(whole);
    whole+=(fraction>=.5f)-(fraction<=-.5f);
    return static_cast<std::int16_t>(whole);
}
// Round the magnitude of the already scaled IEEE single, then restore its
// sign. This uses the same rounded product and ties as QuantizeAudioSample.
void QuantizeAudioBlock(const float* input,std::int16_t* output,unsigned count){
    static_assert(sizeof(float)==sizeof(uint32)&&std::numeric_limits<float>::is_iec559);
    for(unsigned i=0;i<count;++i){
        float scaled=input[i]*32767.f;
        uint32 bits;std::memcpy(&bits,&scaled,sizeof bits);
        const uint32 magnitude=bits&0x7fffffffu;
        unsigned result=0;
        if(magnitude>=0x46fffe00u){ // 32767 or a non-finite value
            result=magnitude<=0x7f800000u?32767:0;
        }else if(magnitude>=0x3f000000u){
            const unsigned shift=150-(magnitude>>23);
            const unsigned significand=0x800000u|(magnitude&0x7fffffu);
            result=(significand+(1u<<(shift-1)))>>shift;
        }
        output[i]=int16_t(bits>>31?-int(result):int(result));
    }
}
AudioMixTicks CurrentAudioMixTicks(){return {audio_timing.fx,audio_timing.wet,audio_timing.streams,audio_timing.blocks};}
void ResetAudioTiming(){audio_timing={};}
void ReportAudioTiming(unsigned frame){
    if(!audio_timing.blocks)return;
    const auto us=[](uint64 ticks){
        return static_cast<unsigned long long>(ScaleClockTicks(ticks,source_clock_frequency(),1000000));
    };
    Log("AUDIO_FRAME n=%u blocks=%u max_refill=%u mix_us=%llu pcm_us=%llu queue_us=%llu fx_us=%llu reverb_us=%llu streams_us=%llu",
        frame,audio_timing.blocks,audio_timing.max_refill,us(audio_timing.mix),us(audio_timing.quantize),us(audio_timing.submit),us(audio_timing.fx),us(audio_timing.wet),us(audio_timing.streams));
}
bool Voice::Next(float* value) {
    if (sample&&!sample->disk&&sample->encoded.empty()) {
        if (sample_at>=sample->mono.size()) {
            if (sample->looping) sample_at=0; else return false;
        }
        value[0]=value[1]=float(sample->mono[sample_at++])/32768.f; return true;
    }
    starved=false;
    if (file_slot>=0) {
        if (block_at==block_frames) {
            block_at=0;
            block_frames=ReadAudioFileBlock(file_slot,block);
            if (!block_frames) {
                const auto state=GetAudioFileStatus(file_slot).state;
                starved=state==AudioFileState::Ready;
                // End or failure may have been published with a last block
                // between the empty read and the status snapshot.
                if (!starved) block_frames=ReadAudioFileBlock(file_slot,block);
                if (!block_frames) return false;
            }
        }
        value[0]=block[block_at*2]; value[1]=block[block_at*2+1]; ++block_at;
        return true;
    }
    if (sample && sample->ima_bytes) {
        if (block_at==block_frames) {
            if (sample_at==sample->ima_bytes) {
                if (!sample->looping) return false;
                sample_at=0;
            }
            if (sample->disk && !disk_bytes) return false;
            const auto& encoded=sample->disk ? *disk_bytes : sample->encoded;
            const auto offset=sample->ima_offset+sample_at;
            if (offset>encoded.size() || encoded.size()-offset<36 ||
                !DecodeEffectBlock(encoded.data()+offset,block,TurnOptions(PerformanceRouteStep())&AudioMix)) return false;
            sample_at+=36;block_at=0;block_frames=65;
        }
        value[0]=value[1]=block[block_at++*2];
        return true;
    }
    if (!stream) return false;
    if (block_at==block_frames) {
        block_at=0;block_frames=stream->Read(block,1024);
        Dbg_MsgAssert(!stream->Failed(),("Original sound decoder failed during playback"));
        if(!block_frames&&sample&&sample->looping){
            Dbg_Assert(stream->Rewind());block_frames=stream->Read(block,1024);Dbg_Assert(!stream->Failed()&&block_frames);
        }
        if(sample)for(std::size_t i=0;i<block_frames;++i){
            // Preserve the previous eager loader's exact mono quantisation.
            const auto mono=QuantizeAudioSample((block[i*2]+block[i*2+1])*.5f);
            block[i*2]=block[i*2+1]=float(mono)/32768.f;
        }
    }
    if (!block_frames) return false;
    value[0]=block[block_at*2]; value[1]=block[block_at*2+1]; ++block_at; return true;
}
void Voice::Begin() {
    if (sample && (sample->disk || !sample->encoded.empty())) {
        RootHeapScope storage;
        const auto* encoded=&sample->encoded;
        if (sample->disk) {
            disk_bytes=sample->active_bytes.lock();
            if (!disk_bytes) {
                const auto& backing=*sample->disk;
                auto* file=File::Open(backing.archive.c_str(),"rb");
                if (!file) { playing=prepared=false; return; }
                auto bytes=std::make_shared<std::vector<uint8>>(backing.size);
                const bool ok=File::Seek(file,backing.offset,SEEK_SET)==0 &&
                    File::Read(bytes->data(),1,bytes->size(),file)==bytes->size();
                File::Close(file);
                if (!ok) { playing=prepared=false; return; }
                disk_bytes=std::move(bytes);
                sample->active_bytes=disk_bytes;
            }
            encoded=disk_bytes.get();
        }
        // The common retail effect needs no FFmpeg probe, packet buffers or
        // decoder allocation while skating. Media streams still use FFmpeg.
        if (!sample->ima_bytes) {
            stream=std::make_unique<MediaDecoder>();
            if (!stream->OpenMemory(encoded->data(),encoded->size())) {
                Log("SFX_DECODER_UNAVAILABLE encoded_bytes=%zu",encoded->size());
                stream.reset();disk_bytes.reset();playing=prepared=false;return;
            }
        }
    }
    sample_at=block_at=block_frames=0; phase=0;
    playing=Next(a); if (!Next(b)) { b[0]=a[0]; b[1]=a[1]; ended=true; } else ended=false;
    prepared=playing;
}
std::size_t Voice::Mix(float* output,std::size_t frames,float gain) {
    if (!playing || paused) return 0;
    if (file_slot>=0 && !primed) {
        const auto status=GetAudioFileStatus(file_slot);
        if (status.state==AudioFileState::Pending || status.state==AudioFileState::Opening ||
            (status.state==AudioFileState::Ready && status.blocks<2)) return 0;
        rate=status.rate;
        Begin();
        primed=true;
        if (!playing) return 0;
    }
    const double step=double(rate)*pitch/(100.0*source_audio_rate());
    const float gain_left=left*gain,gain_right=right*gain;
    // Preserve the interpolation phase across an SD underrun. Other voices
    // still fill this device block and the game never waits for I/O.
    auto advance=[&]() {
        while (phase>=1) {
            if (ended) { playing=false; return false; }
            float next[2];
            if (!Next(next)) {
                if (starved) return false;
                next[0]=b[0]; next[1]=b[1]; ended=true;
            }
            phase-=1;
            a[0]=b[0]; a[1]=b[1];
            b[0]=next[0]; b[1]=next[1];
        }
        return true;
    };
    if (!advance()) return starved ? frames : 0;
    // a and b are the lookahead immediately before the next source frame.
    // Inside a decoded block use a contiguous window. At refill, loop, EOF
    // or SD starvation the same advance() logic owns the phase and lookahead.
    const bool direct=step==1 && phase==0;
    const bool mono=sample&&!sample->disk&&sample->encoded.empty();
    auto mix_window=[&](auto frame,std::size_t& cursor,std::size_t end){
        const auto first=cursor;
        const auto available=end-cursor;
        std::size_t used=0;
        if(direct){
            used=std::min(frames,available);
            for(std::size_t i=0;i<used;++i){
                const auto value=frame(first-2+i);
                output[2*i]+=value[0]*gain_left;
                output[2*i+1]+=value[1]*gain_right;
            }
            frames-=used;
            output+=2*used;
        }else{
            auto left_frame=frame(first-2),right_frame=frame(first-1);
            while(frames && used<available){
                const auto fraction=float(phase);
                output[0]+=(left_frame[0]+(right_frame[0]-left_frame[0])*fraction)*gain_left;
                output[1]+=(left_frame[1]+(right_frame[1]-left_frame[1])*fraction)*gain_right;
                --frames;
                output+=2;
                phase+=step;
                while(phase>=1 && used<available){
                    left_frame=right_frame;
                    right_frame=frame(first+used++);
                    phase-=1;
                }
            }
        }
        cursor+=used;
        const auto previous=frame(cursor-2),next=frame(cursor-1);
        a[0]=previous[0];a[1]=previous[1];
        b[0]=next[0];b[1]=next[1];
    };
    while(frames){
        if(block_at>=2 && block_at<block_frames){
            mix_window([&](std::size_t at){return std::array<float,2>{block[2*at],block[2*at+1]};},
                       block_at,block_frames);
        }else if(mono && sample_at>=2 && sample_at<sample->mono.size()){
            mix_window([&](std::size_t at){const float v=sample->mono[at]/32768.f;return std::array<float,2>{v,v};},
                       sample_at,sample->mono.size());
        }else{
            const float fraction=float(phase);
            output[0]+=(direct?a[0]:a[0]+(b[0]-a[0])*fraction)*gain_left;
            output[1]+=(direct?a[1]:a[1]+(b[1]-a[1])*fraction)*gain_right;
            phase+=step;
            output+=2;
            --frames;
        }
        if(!advance())return starved ? frames : 0;
    }
    return 0;
}
void VolumePair(Sfx::sVolume* volume,float& l,float& r) {
    if (!volume) { l=r=1; return; }
    l=volume->GetChannelVolume(0)*0.01f; r=volume->GetChannelVolume(1)*0.01f;
    if (volume->GetVolumeType()==Sfx::VOLUME_TYPE_5_CHANNEL_DOLBY5_1) {
        // Original channel order FL,FR,BL,BR,C (see original SDL conversion).
        l+=volume->GetChannelVolume(4)*0.007071f+volume->GetChannelVolume(2)*0.007071f;
        r+=volume->GetChannelVolume(4)*0.007071f+volume->GetChannelVolume(3)*0.007071f;
    }
}
void BindAudioSink(AudioSink value) { controller(); Dbg_Assert(!opened && !sink.owner); Dbg_Assert(value.owner&&value.open&&value.available&&value.submit&&value.close); sink=value; }
void UnbindAudioSink(void* owner) { controller(); Dbg_Assert(!opened && owner==sink.owner); sink={}; }
bool AudioReady() { controller(); return opened; }
void OpenAudio() {
    controller(); if (opened) return;
    bool ok=sink.owner?sink.open(sink.owner):source_audio_open()==0;
    Dbg_MsgAssert(ok,("Native NDSP audio initialization failed errno=%d",errno));
    const bool worker_ready=StartAudioFileWorker();
    Dbg_MsgAssert(worker_ready,("Native audio stream worker initialization failed errno=%d",errno));
    opened=true;
}
void SetNativeReverb(float level,int mode,bool instant) {
    controller(); Dbg_Assert(std::isfinite(level) && level>=0 && level<=100 && mode>=0);
    reverb.target=mode==20 || source_application_os_is_old_3ds() ? 0 : level*.01f;
    reverb.delay=1200+(unsigned(mode)%10)*480;
    if (instant) reverb.level=reverb.target;
}
void RenderAudio(float* output,std::size_t frames) {
    controller(); Dbg_Assert(output && frames<=SOURCE_AUDIO_FRAMES);
    std::fill(output,output+frames*2,0.f);
    const bool sampled=logic_timing_active&&logic_timing_detailed;
    const auto start=sampled?source_clock_now():0;
    MixEffects(output,frames);
    const auto fx=sampled?source_clock_now():0;
    if(!source_application_os_is_old_3ds()){
        if(TurnOptions(PerformanceRouteStep())&AudioMix)reverb.Block(output,frames);
        else reverb.Scalar(output,frames);
    }
    const auto wet=sampled?source_clock_now():0;
    MixStreams(output,frames);
    if(sampled){
        audio_timing.fx+=fx-start;
        audio_timing.wet+=wet-fx;
        audio_timing.streams+=source_clock_now()-wet;
    }
}
void PumpAudio() {
    ScopedLogicTiming timing(LogicArea::Audio);
    controller(); Dbg_Assert(opened);
    const bool sampled=logic_timing_active&&logic_timing_detailed;
    const int free_slots=sink.owner?SOURCE_AUDIO_BUFFERS:source_audio_available();
    Dbg_MsgAssert(free_slots>=0,("NDSP queue query failed"));
    // Fill from a single queue snapshot, so time spent mixing cannot
    // increase the same pump's workload. Keep headroom of three buffers
    // after a late pump and fill all four on a normal refill.
    const int refill=sink.owner?free_slots:
        (free_slots<=2?free_slots:(free_slots==SOURCE_AUDIO_BUFFERS?3:2));
    unsigned submitted=0;
    for (int batch=0;batch<refill;++batch) {
        if(sink.owner&&!sink.available(sink.owner))break;
        float mixed[SOURCE_AUDIO_FRAMES*2]; std::int16_t pcm[SOURCE_AUDIO_FRAMES*2];
        const auto start=sampled?source_clock_now():0;
        RenderAudio(mixed,SOURCE_AUDIO_FRAMES);
        const auto mixed_at=sampled?source_clock_now():0;
        if(UseOptimizedFramePath(PerformanceRouteStep()))QuantizeAudioBlock(mixed,pcm,SOURCE_AUDIO_FRAMES*2);
        else for(unsigned i=0;i<SOURCE_AUDIO_FRAMES*2;++i)pcm[i]=QuantizeAudioSample(mixed[i]);
        const auto converted_at=sampled?source_clock_now():0;
        bool ok=sink.owner?sink.submit(sink.owner,pcm,SOURCE_AUDIO_FRAMES):source_audio_submit(pcm,SOURCE_AUDIO_FRAMES)==0;
        Dbg_MsgAssert(ok,("NDSP PCM submission failed errno=%d",errno));
        if(sampled){
            audio_timing.mix+=mixed_at-start;
            audio_timing.quantize+=converted_at-mixed_at;
            audio_timing.submit+=source_clock_now()-converted_at;
            ++submitted;
        }
    }
    audio_timing.blocks+=submitted;
    audio_timing.max_refill=std::max(audio_timing.max_refill,submitted);
}
void CloseAudio() {
    controller(); if (!opened) return;
    StopNativeStreams(); StopAudioFileWorker(); ReleasePermanentAudio(); ResetNativeStreamCatalogs();
    if (sink.owner) sink.close(sink.owner); else source_audio_close();
    opened=false; reverb.left.fill(0); reverb.right.fill(0);
    reverb.cursor=0; reverb.level=reverb.target=0;
}
}
