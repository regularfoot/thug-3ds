#pragma once
#include <core/defines.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cmath>
namespace SourcePort {
struct ResourceError { std::size_t offset=0; const char* reason=nullptr;const char* context=nullptr; };
// PC retail files are little-endian, unaligned wire data. No host struct casts.
class ResourceReader {
    const uint8* data_;std::size_t size_,at_=0;ResourceError* error_;bool ok_=true;const char* context_=nullptr;
    using Fill = bool (*)(void*, uint8*, std::size_t);
    Fill fill_ = nullptr;
    void* source_ = nullptr;
    uint8* buffer_ = nullptr;
    std::size_t capacity_ = 0, buffered_ = 0, cursor_ = 0;
    const uint8* StreamBytes(std::size_t n) {
        const auto available = buffered_ - cursor_;
        if (n > available) {
            if (n > capacity_) { Fail("resource read exceeds stream buffer"); return nullptr; }
            if (available) std::memmove(buffer_, buffer_ + cursor_, available);
            buffered_ = std::min(capacity_, size_ - at_);
            cursor_ = 0;
            if (!fill_(source_, buffer_ + available, buffered_ - available)) {
                Fail("incomplete resource stream");
                return nullptr;
            }
        }
        const auto* result = buffer_ + cursor_;
        cursor_ += n;
        return result;
    }
public:
    ResourceReader(const void* data,std::size_t size,ResourceError* error):data_(static_cast<const uint8*>(data)),size_(size),error_(error){if(error_)*error_={};if(!data&&size)Fail("null resource");}
    ResourceReader(std::size_t size, ResourceError* error, Fill fill, void* source, uint8* buffer, std::size_t capacity)
        : ResourceReader(buffer, size, error) {
        fill_ = fill; source_ = source; buffer_ = buffer; capacity_ = capacity;
    }
    bool Fail(const char* reason){if(ok_&&error_)*error_={at_,reason,context_};ok_=false;return false;}
    void Context(const char* context){context_=context;}
    bool Ok()const{return ok_;} std::size_t Position()const{return at_;}std::size_t Remaining()const{return ok_?size_-at_:0;}
    const uint8* Bytes(std::size_t n){if(!ok_||n>size_-at_){Fail("truncated resource");return nullptr;}auto* p=fill_?StreamBytes(n):data_+at_;if(p)at_+=n;return p;}
    bool Skip(std::size_t n) {
        if (!fill_) return Bytes(n) != nullptr;
        if (!ok_ || n > Remaining()) return Fail("truncated resource");
        while (n) {
            const auto chunk = std::min(capacity_, n);
            if (!Bytes(chunk)) return false;
            n -= chunk;
        }
        return ok_;
    }
    uint8 U8(){auto* p=Bytes(1);return p?p[0]:0;}
    uint16 U16(){auto* p=Bytes(2);return p?uint16(p[0]|(uint16(p[1])<<8)):0;}
    uint32 U32(){auto* p=Bytes(4);return p?uint32(p[0])|(uint32(p[1])<<8)|(uint32(p[2])<<16)|(uint32(p[3])<<24):0;}
    uint64 U64(){uint64 lo=U32(),hi=U32();return lo|(hi<<32);}
    int I32(){uint32 u=U32();std::int32_t v;std::memcpy(&v,&u,4);return v;}
    float RawF32(){uint32 u=U32();float f;std::memcpy(&f,&u,4);return f;}
    float F32(){float f=RawF32();if(!std::isfinite(f))Fail("nonfinite resource float");return f;}
    bool Count(std::size_t count,std::size_t stride,std::size_t max){return (ok_&&count<=max&&stride&&count<=Remaining()/stride)||Fail("invalid resource count");}
};
}
