#include <source_port/media_os.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mem.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
_Static_assert(SOURCE_MEDIA_SEEK_SIZE==AVSEEK_SIZE && SOURCE_MEDIA_SEEK_FORCE==AVSEEK_FORCE,"FFmpeg seek ABI changed");
_Static_assert(sizeof(enum AVSampleFormat)==sizeof(int),"FFmpeg requires its 32-bit enum ABI");
struct SourceMediaCodec {
    SourceMediaIO source;
    AVFormatContext* format;
    AVIOContext* io;
    AVCodecContext* codec;
    AVPacket* packet;
    AVFrame* frame;
    int stream,frame_at,pending,draining,ended,failed;
};
static int read_data(void* opaque,uint8_t* buffer,int bytes){
    SourceMediaCodec* s=opaque;int n=s->source.read(s->source.owner,buffer,bytes);
    return n>bytes?AVERROR(EIO):n?n:AVERROR_EOF;
}
static int64_t seek_data(void* opaque,int64_t offset,int origin){
    SourceMediaCodec* s=opaque;return s->source.seek(s->source.owner,offset,origin);
}
void source_media_close(SourceMediaCodec* s){
    if(!s)return;
    av_frame_free(&s->frame);av_packet_free(&s->packet);avcodec_free_context(&s->codec);
    avformat_close_input(&s->format);
    if(s->io)av_freep(&s->io->buffer);
    avio_context_free(&s->io);free(s);
}
// Set FFmpeg's process-wide log level before any playback threads exist.
__attribute__((constructor)) static void configure_media_logging(void) {
    av_log_set_level(AV_LOG_ERROR);
}
SourceMediaCodec* source_media_open(SourceMediaIO io){
    if(!io.owner||!io.read||!io.seek){errno=EINVAL;return NULL;}
    SourceMediaCodec* s=calloc(1,sizeof(*s));if(!s)return NULL;s->source=io;
    uint8_t* buffer=av_malloc(32768);if(!buffer)goto failed;
    s->io=avio_alloc_context(buffer,32768,0,s,read_data,NULL,seek_data);
    if(!s->io){av_free(buffer);goto failed;}
    s->format=avformat_alloc_context();if(!s->format)goto failed;
    s->format->pb=s->io;s->format->flags|=AVFMT_FLAG_CUSTOM_IO;
    s->format->probesize=256*1024;s->format->max_analyze_duration=AV_TIME_BASE;
    if(avformat_open_input(&s->format,NULL,NULL,NULL)<0)goto failed;
    // The Bink header already describes every stream. Probing would also open
    // the full-size video decoder inside this independent audio reader.
    if(s->format->iformat!=av_find_input_format("bink") && avformat_find_stream_info(s->format,NULL)<0)goto failed;
    const AVCodec* codec=NULL;
    s->stream=av_find_best_stream(s->format,AVMEDIA_TYPE_AUDIO,-1,-1,&codec,0);
    if(s->stream<0||!codec)goto failed;
    s->codec=avcodec_alloc_context3(codec);if(!s->codec)goto failed;
    if(avcodec_parameters_to_context(s->codec,s->format->streams[s->stream]->codecpar)<0)goto failed;
    s->codec->thread_count=1;
    if(s->codec->sample_rate<8000||s->codec->sample_rate>96000||s->codec->ch_layout.nb_channels<1||s->codec->ch_layout.nb_channels>2)goto failed;
    if(avcodec_open2(s->codec,codec,NULL)<0)goto failed;
    s->frame=av_frame_alloc();s->packet=av_packet_alloc();
    if(!s->frame||!s->packet)goto failed;
    return s;
failed:
    source_media_close(s);errno=EILSEQ;return NULL;
}
unsigned source_media_rate(const SourceMediaCodec* s){return s?(unsigned)s->codec->sample_rate:0;}
int source_media_rewind(SourceMediaCodec* s){
    if(!s||av_seek_frame(s->format,s->stream,0,AVSEEK_FLAG_BACKWARD)<0){errno=EIO;return -1;}
    avcodec_flush_buffers(s->codec);av_packet_unref(s->packet);av_frame_unref(s->frame);
    s->frame_at=s->pending=s->draining=s->ended=s->failed=0;return 0;
}
int source_media_ended(const SourceMediaCodec* s){return !s||s->ended;}
int source_media_failed(const SourceMediaCodec* s){return !s||s->failed;}
size_t source_media_read(SourceMediaCodec* s,float* output,size_t frames){
    if(!s||s->failed||(!output&&frames))return 0;
    size_t produced=0;
    while(produced<frames&&!s->ended&&!s->failed){
        if(s->frame_at<s->frame->nb_samples){
            int channels=s->frame->ch_layout.nb_channels;
            enum AVSampleFormat format=s->frame->format;
            enum AVSampleFormat packed=av_get_packed_sample_fmt(format);
            int planar=av_sample_fmt_is_planar(format);
            if((packed!=AV_SAMPLE_FMT_FLT&&packed!=AV_SAMPLE_FMT_S16)||channels<1||channels>2){s->failed=1;break;}
            size_t count=(size_t)(s->frame->nb_samples-s->frame_at);
            if(count>frames-produced)count=frames-produced;
            // One format/plane decision per decoded block. The previous loop
            // queried FFmpeg's sample metadata twice for EVERY audio frame.
            // Preserve the exact PCM conversion, duplication and NaN policy.
            for(int c=0;c<2;++c){
                int channel=c<channels?c:channels-1;
                const uint8_t* bytes=s->frame->extended_data[planar?channel:0];
                size_t stride=planar?1:(size_t)channels,index=(size_t)s->frame_at*stride+(planar?0:channel);
                float* target=output+produced*2+c;
                if(packed==AV_SAMPLE_FMT_FLT){
                    for(size_t i=0;i<count;++i){float value;memcpy(&value,bytes+(index+i*stride)*sizeof(float),sizeof(value));target[i*2]=isfinite(value)?value:0;}
                }else{
                    for(size_t i=0;i<count;++i){int16_t sample;memcpy(&sample,bytes+(index+i*stride)*sizeof(sample),sizeof(sample));target[i*2]=(float)sample/32768.f;}
                }
            }
            s->frame_at+=(int)count;produced+=count;continue;
        }
        av_frame_unref(s->frame);s->frame_at=0;
        int result=avcodec_receive_frame(s->codec,s->frame);
        if(result==0)continue;
        if(result==AVERROR_EOF){s->ended=1;break;}
        if(result!=AVERROR(EAGAIN)){s->failed=1;break;}
        if(s->pending){
            result=avcodec_send_packet(s->codec,s->packet);
            if(result==AVERROR(EAGAIN))continue;
            av_packet_unref(s->packet);s->pending=0;
            if(result<0)s->failed=1;
        }else if(!s->draining){
            result=av_read_frame(s->format,s->packet);
            if(result==AVERROR_EOF){s->draining=1;if(avcodec_send_packet(s->codec,NULL)<0)s->failed=1;}
            else if(result<0)s->failed=1;
            else if(s->packet->stream_index==s->stream)s->pending=1;
            else av_packet_unref(s->packet);
        }else s->failed=1;
    }
    return produced;
}
