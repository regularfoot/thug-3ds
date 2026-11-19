#include <core/defines.h>
#include <source_port/media.h>
#include <source_port/media_os.h>
#include <source_port/thread_os.h>
#include <sys/file/filesys.h>
#include <algorithm>
namespace SourcePort {
struct MediaDecoder::State {
    void* file=nullptr;
    const std::uint8_t* memory=nullptr;
    std::int64_t start=0,length=0,position=0;
    SourceMediaCodec* codec=nullptr;
    static int read(void* opaque,std::uint8_t* buffer,int requested) {
        auto& s=*static_cast<State*>(opaque);
        if(requested<0)return -EINVAL;
        auto bytes=std::min<std::int64_t>(requested,s.length-s.position);
        if(bytes<=0)return 0;
        std::size_t n;
        if(s.memory){n=bytes;std::memcpy(buffer,s.memory+s.position,n);}
        else n=File::Read(buffer,1,std::size_t(bytes),s.file);
        s.position+=n;return n?int(n):-EIO;
    }
    static std::int64_t seek(void* opaque,std::int64_t offset,int origin) {
        auto& s=*static_cast<State*>(opaque);
        if(origin==SOURCE_MEDIA_SEEK_SIZE)return s.length;
        origin&=~SOURCE_MEDIA_SEEK_FORCE;
        std::int64_t base;
        switch(origin){case SEEK_SET:base=0;break;case SEEK_CUR:base=s.position;break;case SEEK_END:base=s.length;break;default:return -EINVAL;}
        if(offset < -base || offset > s.length-base)return -EINVAL;
        auto at=base+offset;
        if(!s.memory&&File::Seek(s.file,long(s.start+at),SEEK_SET))return -EIO;
        s.position=at;return at;
    }
    ~State(){source_media_close(codec);if(file)File::Close(file);}
};
MediaDecoder::~MediaDecoder(){Close();}
void MediaDecoder::Close(){Dbg_Assert(source_thread_is_controller());delete state_;state_=nullptr;}
bool MediaDecoder::Open(const char* name,std::uint64_t offset,std::uint64_t length){
    Close();failed_=false;auto* s=new State;
    auto abort=[&]{delete s;failed_=true;return false;};
    s->file=File::Open(name,"rb");if(!s->file)return abort();
    auto size=File::GetFileSize(s->file);
    if(size<=0||offset>std::uint64_t(size))return abort();
    if(!length)length=std::uint64_t(size)-offset;
    if(!length||length>std::uint64_t(size)-offset||length>INT_MAX)return abort();
    s->start=offset;s->length=length;
    if(File::Seek(s->file,long(offset),SEEK_SET))return abort();
    s->codec=source_media_open({s,State::read,State::seek});if(!s->codec)return abort();
    state_=s;return true;
}
bool MediaDecoder::OpenMemory(const std::uint8_t* bytes,std::size_t length){
    Close();failed_=false;
    if(!bytes||!length||length>INT_MAX){failed_=true;return false;}
    auto* s=new State;s->memory=bytes;s->length=length;
    s->codec=source_media_open({s,State::read,State::seek});
    if(!s->codec){delete s;failed_=true;return false;}state_=s;return true;
}
bool MediaDecoder::Rewind(){
    Dbg_Assert(source_thread_is_controller());
    if(!state_||source_media_rewind(state_->codec)){failed_=true;return false;}
    failed_=false;return true;
}
unsigned MediaDecoder::Rate()const{return state_?source_media_rate(state_->codec):0;}
bool MediaDecoder::Ended()const{return !state_||source_media_ended(state_->codec);}
bool MediaDecoder::Failed()const{return failed_||(state_&&source_media_failed(state_->codec));}
std::size_t MediaDecoder::Read(float* output,std::size_t frames){
    Dbg_Assert(source_thread_is_controller());
    if(!state_||failed_||(!output&&frames))return 0;
    return source_media_read(state_->codec,output,frames);
}
}
