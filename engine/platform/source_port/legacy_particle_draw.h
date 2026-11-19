#pragma once
#include <source_port/native_particles.h>
#include <gfx/nxparticle.h>

namespace SourcePort {
enum class ParticlePrimitive { Quads, Lines, Strip, Fan };

// One original emitter's material state and immediate vertex stream. End()
// converts the actual console primitive topology to native triangles/lines;
// all simulation, history buffers and shape calculations stay in the source.
class ParticleDrawScope {
public:
    ParticleDrawScope(Nx::CNativeTexture*, unsigned blend, unsigned fixed_alpha);
    ~ParticleDrawScope();
    ParticleDrawScope(const ParticleDrawScope&) = delete;
    ParticleDrawScope& operator=(const ParticleDrawScope&) = delete;
private:
    friend struct ParticleEmit;
    WorldBatch batch_;
    std::vector<SourceRenderVertex> vertices_;
    ParticlePrimitive primitive_ = ParticlePrimitive::Quads;
    std::size_t expected_ = 0;
    bool begun_ = false, textured_ = false, pending_ = false;
    bool color_ = false, uv_ = false;
    void finish_vertex();
};
struct ParticleEmit {
    static void Textured(bool);
    static void Begin(ParticlePrimitive, int count);
    static void Position3f32(float, float, float);
    static void Color(Image::RGBA);
    static void TexCoord2f32(float, float);
    static void End();
};
}
