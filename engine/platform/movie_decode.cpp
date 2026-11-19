#include <source_port/movies.h>
#include <source_port/diagnostics.h>
#include <cerrno>
#ifdef __3DS__
#include <source_port/memory.h>
#include <source_port/thread_os.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
static_assert(sizeof(AVPixelFormat)==sizeof(int),"FFmpeg requires 32-bit enums");
#include <algorithm>
#include <memory>
#include <string>

#ifdef __3DS__
// Called synchronously by the local Bink decoder. The codec holds the
// AVBufferRef until close, including failed and skipped movie opens.
extern "C" AVBufferRef* rethpsu_bink_workspace(std::size_t bytes) {
    if (!SourcePort::EngineMemoryIsReady() || !source_thread_is_controller()) return nullptr;
    auto storage = std::unique_ptr<std::uint8_t[]>(new (std::nothrow) std::uint8_t[bytes]());
    if (!storage) return nullptr;
    auto release = [](void*, std::uint8_t* memory) { delete[] memory; };
    AVBufferRef* buffer = av_buffer_create(storage.get(), bytes, release, nullptr, 0);
    if (buffer) storage.release();
    return buffer;
}
#endif

namespace SourcePort {
struct MovieVideo::State {
    AVFormatContext* format=nullptr;
    AVCodecContext* codec=nullptr;
    AVPacket* packet=nullptr;
    AVFrame* frame=nullptr;
    AVRational fps{};
    int stream=-1;
    bool draining=false;
    ~State(){
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
};

MovieVideo::~MovieVideo(){delete state_;}

bool MovieVideo::Open(const char* path){
    if(state_||!path)return false;
    auto s=std::make_unique<State>();
    const std::string filename=std::string("file:")+path;
    auto* bink=av_find_input_format("bink");
    if(!bink)return false;
    auto error = [&](const char* stage,int code){
        Log("MOVIE_OPEN_ERROR path=%s stage=%s code=%d errno=%d",path,stage,code,errno);
        return false;
    };
    int result=avformat_open_input(&s->format,filename.c_str(),bink,nullptr);
    if(result<0)return error("demux",result);
    const AVCodec* decoder=nullptr;
    s->stream=av_find_best_stream(s->format,AVMEDIA_TYPE_VIDEO,-1,-1,&decoder,0);
    if(s->stream<0||!decoder)return error("stream",s->stream);
    auto* stream=s->format->streams[s->stream];
    if(stream->codecpar->codec_id!=AV_CODEC_ID_BINKVIDEO||stream->codecpar->width<=0||stream->codecpar->height<=0||
       stream->codecpar->width>1024||stream->codecpar->height>768)return error("source-format",stream->codecpar->codec_id);
    s->fps=stream->avg_frame_rate.num?stream->avg_frame_rate:av_inv_q(stream->time_base);
    const double fps=av_q2d(s->fps);
    if(!(fps>=1&&fps<=60))return error("frame-rate",s->fps.num);
    s->codec=avcodec_alloc_context3(decoder);
    if(!s->codec)return error("codec-alloc",-1);
    result=avcodec_parameters_to_context(s->codec,stream->codecpar);
    if(result<0)return error("codec-params",result);
    s->codec->thread_count=1;
    result=avcodec_open2(s->codec,decoder,nullptr);
    if(result<0)return error("decoder",result);
    s->packet=av_packet_alloc();
    s->frame=av_frame_alloc();
    if(!s->packet||!s->frame)return error("frame-alloc",-1);
    state_=s.release();
    return true;
}

int MovieVideo::Read(){
    auto& s=*state_;
    av_frame_unref(s.frame);
    for(;;){
        int result=avcodec_receive_frame(s.codec,s.frame);
        if(result==0){
            if(s.frame->format!=AV_PIX_FMT_YUV420P&&s.frame->format!=AV_PIX_FMT_YUVA420P)return -1;
            return 1;
        }
        if(result==AVERROR_EOF)return 0;
        if(result!=AVERROR(EAGAIN)||s.draining)return -1;
        while((result=av_read_frame(s.format,s.packet))>=0&&s.packet->stream_index!=s.stream)
            av_packet_unref(s.packet);
        if(result==AVERROR_EOF){
            s.draining=true;
            if(avcodec_send_packet(s.codec,nullptr)<0)return -1;
        }else{
            if(result<0)return -1;
            result=avcodec_send_packet(s.codec,s.packet);
            av_packet_unref(s.packet);
            if(result<0)return -1;
        }
    }
}

std::uint64_t MovieVideo::FrameTicks(std::uint64_t hz)const{
    return hz*unsigned(state_->fps.den)/unsigned(state_->fps.num);
}

void MovieVideo::Tile(std::vector<std::uint8_t>& pixels)const{
    const auto& frame=*state_->frame;
    pixels.resize(TextureWidth*TextureHeight*4);
    unsigned source_x[Width];
    for(unsigned x=0;x<Width;++x)source_x[x]=x*unsigned(frame.width)/Width;
    const bool full=frame.color_range==AVCOL_RANGE_JPEG;
    constexpr unsigned morton_x[]={0,1,4,5,16,17,20,21};
    constexpr unsigned morton_y[]={0,2,8,10,32,34,40,42};
    auto channel=[](int value){return std::uint8_t(std::clamp((value+128)>>8,0,255));};
    for(unsigned y=0;y<Height;++y){
        const unsigned sy=y*unsigned(frame.height)/Height;
        const auto* luma=frame.data[0]+sy*frame.linesize[0];
        const auto* cb=frame.data[1]+(sy/2)*frame.linesize[1];
        const auto* cr=frame.data[2]+(sy/2)*frame.linesize[2];
        auto* row=pixels.data()+(y/8)*(TextureWidth/8)*256+morton_y[y%8]*4;
        for(unsigned x=0;x<Width;++x){
            const auto sx=source_x[x];
            const int yy=full?256*luma[sx]:298*(int(luma[sx])-16);
            const int u=int(cb[sx/2])-128,v=int(cr[sx/2])-128;
            auto* texel=row+(x/8)*256+morton_x[x%8]*4;
            texel[0]=255;
            texel[1]=channel(yy+(full?454:516)*u);
            texel[2]=channel(yy-(full?88:100)*u-(full?183:208)*v);
            texel[3]=channel(yy+(full?359:409)*v);
        }
    }
}
}
