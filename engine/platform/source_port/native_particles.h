#pragma once
#include <source_port/native_world.h>
#include <gfx/nxnewparticlemgr.h>
#include <deque>
namespace SourcePort {
bool VancouverFountain(uint32 node,uint32 texture);
unsigned OriginalParticleBlend(uint32);
class EffectRenderScope {
public:
    EffectRenderScope(const WorldView&,std::vector<WorldBatch>&);~EffectRenderScope();
    EffectRenderScope(const EffectRenderScope&)=delete;EffectRenderScope& operator=(const EffectRenderScope&)=delete;
    static const WorldView& View();static std::vector<WorldBatch>& Output();
private:const WorldView* view_;std::vector<WorldBatch>* output_;EffectRenderScope* previous_;
};
struct ParticleSample {Mth::Vector position;float radius=0;Image::RGBA color;};
}
namespace Nx {
class CNativeNewParticle final:public CNewParticle {
public:
    ~CNativeNewParticle()override;
    void Advance(float dt);
    std::vector<SourcePort::ParticleSample> Samples()const;
    void Gather(const SourcePort::WorldView&,std::vector<SourcePort::WorldBatch>&)const;
private:
    struct Stream {float age=0,rate=0,interval=0;int count=0;uint32 seed=1;};
    std::deque<Stream> streams_;bool built_=false,emitting_=false,hidden_=false;
    void plat_build()override;void plat_destroy()override;void plat_update()override;void plat_render()override;void plat_hide(bool)override;
};
class CNativeNewParticleManager final:public CNewParticleManager {
private:CNewParticle* plat_create_particle()override;
};
}
