#include <source_port/legacy_particle_draw.h>
#include <gfx/ngc/p_nxparticleline.h>
#include <gfx/ngc/p_nxparticleflat.h>
#include <gfx/ngc/p_nxparticleshaded.h>
#include <gfx/ngc/p_nxparticlesmooth.h>
#include <gfx/ngc/p_nxparticleglow.h>
#include <gfx/ngc/p_nxparticlestar.h>
#include <gfx/ngc/p_nxparticlesmoothstar.h>
#include <gfx/ngc/p_nxparticleribbon.h>
#include <gfx/ngc/p_nxparticlesmoothribbon.h>
#include <gfx/ngc/p_nxparticleribbontrail.h>
#include <gfx/ngc/p_nxparticleglowribbontrail.h>
#include <cmath>

namespace {
template<class Shape> class NativeShape final : public Shape {
public: using Shape::Shape;
private:
    void plat_build_path() override {
        // These original shapes evaluate their path from live history/size
        // data in plat_render. There is no cached GPU parametric path to build.
        Dbg_Assert(this->m_max_particles > 0 && this->m_num_particles >= 0 && this->m_num_particles <= this->m_max_particles);
        Dbg_Assert(std::isfinite(this->m_sw) && std::isfinite(this->m_sh) && std::isfinite(this->m_ew) && std::isfinite(this->m_eh));
    }
};
}
namespace Nx {
CParticle* plat_create_particle(uint32 checksum,uint32 type,int maximum,int streams,uint32 texture,uint32 blend,int fixed,int segments,float split,int history) {
    Dbg_Assert(maximum > 0 && maximum <= 1024*1024 && fixed >= 0 && fixed <= 255 && history >= 0 && history <= 1024 && std::isfinite(split));
    // max_streams belongs to the separate original CNewParticle subsystem.
    (void)streams;
    switch(type) {
    case 0x2eeb4b09: return new NativeShape<CNgcParticleLine>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0xaab555bb: case 0xdedfc057: return new NativeShape<CNgcParticleFlat>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0xf4d8d486: return new NativeShape<CNgcParticleShaded>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0x8addac1f: return new NativeShape<CNgcParticleSmooth>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0x15834eea: Dbg_Assert(segments >= 3 && segments <= 1024); return new NativeShape<CNgcParticleGlow>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0x3624a5eb: Dbg_Assert(segments >= 3 && segments <= 1024); return new NativeShape<CNgcParticleStar>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0x097cb7a9: Dbg_Assert(segments >= 3 && segments <= 1024); return new NativeShape<CNgcParticleSmoothStar>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0x0ee6fc5b: return new NativeShape<CNgcParticleRibbon>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0x3f109fcc: return new NativeShape<CNgcParticleSmoothRibbon>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0xc4d5a4cb: Dbg_Assert(history >= 1); return new NativeShape<CNgcParticleRibbonTrail>(checksum,maximum,texture,blend,fixed,segments,split,history);
    case 0x7ec7252d: Dbg_Assert(history >= 1 && segments >= 3 && segments <= 1024); return new NativeShape<CNgcParticleGlowRibbonTrail>(checksum,maximum,texture,blend,fixed,segments,split,history);
    default: Dbg_MsgAssert(false,("Unknown original particle shape %08x",type)); return nullptr;
    }
}
}
