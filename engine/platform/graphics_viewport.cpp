#include <core/defines.h>
#include <gfx/nxviewman.h>
#include <gfx/nxviewport.h>
#include <source_port/graphics.h>
#include <algorithm>
namespace Nx {
// Real original CViewport/Camera/smart-pointer ownership and 2D scale policy.
// Only the platform projection is replaced; no substitute camera controller.
class CNativeViewport final:public CViewport {
public:
    CNativeViewport(const Mth::Rect* rect,Gfx::Camera* camera):CViewport(rect,camera) {}
private:
    float plat_transform_to_screen_coord(const Mth::Vector& world,float& x,float& y,ZBufferValue& z) override {
        auto* camera=GetCamera();if (!camera) return -1;
        float nx,ny,depth,distance;
        if (!SourcePort::ProjectWorld(camera->GetMatrix(),camera->GetPos(),world,camera->GetAdjustedHFOV(),GetAspectRatio(),
            camera->GetNearClipPlane(),camera->GetFarClipPlane(),nx,ny,depth,distance)) return -1;
        x=(GetOriginX()+(nx+1)*0.5f*GetWidth())*640.f;
        y=(GetOriginY()+(1-ny)*0.5f*GetHeight())*448.f;
        z=uint32(depth*65536.f);
        return std::clamp(CViewportManager::sGet2DIn3DSpaceNoscaleDistance()/distance,
            CViewportManager::sGet2DIn3DSpaceMinScale(),CViewportManager::sGet2DIn3DSpaceMaxScale());
    }
};
CViewport* CViewportManager::s_plat_create_viewport(const Mth::Rect* rect,Gfx::Camera* camera) { return new CNativeViewport(rect,camera); }
}
