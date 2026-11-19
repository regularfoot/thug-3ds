#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <type_traits>
namespace SourcePort {
struct Reverb {
    std::array<float,8192> left{},right{};
    unsigned cursor=0,delay=2400;
    float level=0,target=0;
    void Scalar(float* samples,std::size_t frames) {
        for (std::size_t i=0;i<frames;++i) {
            level += (target-level)*0.001f;
            unsigned at=(cursor+8192-delay)%8192;
            float l=left[at],r=right[at];
            left[cursor]=samples[i*2]+r*0.65f; right[cursor]=samples[i*2+1]+l*0.65f;
            samples[i*2]+=l*level; samples[i*2+1]+=r*level;
            cursor=(cursor+1)%8192;
        }
    }

    // Process contiguous parts of both circular buffers. A read never
    // overtakes its write, including a block that spans either ring end.
    void Block(float* samples,std::size_t frames){
        const unsigned size=left.size();
        while(frames){
            const unsigned at=(cursor+size-delay)%size;
            const unsigned n=std::min<std::size_t>({frames,size-at,size-cursor,delay});
            auto apply=[&](auto moving){
                for(unsigned i=0;i<n;++i){
                    if constexpr(decltype(moving)::value)level+=(target-level)*.001f;
                    const float l=left[at+i],r=right[at+i];
                    left[cursor+i]=samples[i*2]+r*.65f;
                    right[cursor+i]=samples[i*2+1]+l*.65f;
                    samples[i*2]+=l*level;samples[i*2+1]+=r*level;
                }
            };
            if(level==target)apply(std::false_type{});
            else apply(std::true_type{});
            cursor=(cursor+n)%size;samples+=2*n;frames-=n;
        }
    }
};
}
