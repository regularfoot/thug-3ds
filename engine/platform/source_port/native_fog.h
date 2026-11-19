#pragma once
#include <source_port/graphics_os.h>
#include <gfx/nxmiscfx.h>
namespace SourcePort {
// Original PC fog: linear range of 600 feet after the scripted near distance;
// RGBA alpha/128 is density. Its obsolete exponent setter has no visual effect.
struct FogSettings {bool enabled=false;float start=3000,exponent=10;Image::RGBA color{0x60,0x60,0x80,0x66};};
const FogSettings& NativeFog();
void ApplyNativeFog(SourceDrawPacket&,bool sky);
}
