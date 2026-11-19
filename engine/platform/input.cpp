#include <core/defines.h>
#include <source_port/input.h>
#include <source_port/launch_paths.h>
#include <source_port/thread_os.h>
#include <algorithm>
#include <cerrno>
namespace {
SourcePort::InputSource source{};
bool opened = false, connected = false;
bool route_active = false;
unsigned route_step = 0;
unsigned menu_test_frame = 0;
unsigned char packet[32];
void controller() { Dbg_Assert(source_thread_is_controller()); }
unsigned char axis(int value, bool invert) {
    // HID/IRRST signed axes are positive up/right, PS2 packet positive down/right.
    value = std::clamp(value, -156, 156);
    if (invert) value = -value;
    // Inp::Data retains the original radial/axial tolerance. Do not double it.
    return static_cast<unsigned char>(value < 0 ? 128 + value * 128 / 156
                                               : 128 + value * 127 / 156);
}
}
namespace SourcePort {
void BeginCityMenuTrial(){if(CompareStereoCityTurns())menu_test_frame=1;}
void StartPerformanceRoute() { controller(); route_step = 0; route_active = true; }
void StopPerformanceRoute() { controller(); route_active = false; route_step = 0; }
unsigned PerformanceRouteStep() { return route_step; }

void EncodeInputPacket(const SourceInputSample& sample, unsigned char* out) {
    Dbg_Assert(out);
    std::memset(out, 0, 32);
    out[0] = sample.connected ? 0 : 0xff;
    out[1] = 0x73; // actual analog controller; pressure buttons unsupported
    auto held = sample.connected ? sample.buttons : 0;
    out[2] = static_cast<unsigned char>(~(held >> 8));
    out[3] = static_cast<unsigned char>(~held);
    out[4] = sample.connected ? axis(sample.right_x, false) : 128;
    out[5] = sample.connected ? axis(sample.right_y, true) : 128;
    out[6] = sample.connected ? axis(sample.left_x, false) : 128;
    out[7] = sample.connected ? axis(sample.left_y, true) : 128;
}
void BindInputSource(InputSource provider) {
    controller(); Dbg_Assert(!opened && !source.owner);
    Dbg_Assert(provider.owner && provider.open && provider.read && provider.close);
    source = provider;
}
void UnbindInputSource(void* owner) {
    controller(); Dbg_Assert(!opened && owner && source.owner == owner);
    source = {};
}
void OpenInput() {
    controller(); Dbg_Assert(!opened);
    bool success = source.owner ? source.open(source.owner) : source_input_open() == 0;
    Dbg_MsgAssert(success, ("Native input unavailable errno=%d", errno));
    opened = true;
    RefreshInput();
}
void RefreshInput() {
    controller(); Dbg_Assert(opened);
    SourceInputSample sample{};
    bool success = source.owner ? source.read(source.owner, &sample) : source_input_read(&sample) == 0;
    // A failed read invalidates the device; never retain stuck held buttons.
    if (!success) sample = {};
    if(menu_test_frame && sample.connected){
        // Exercise Free Skate and its ordinary menu transitions from the test save.
        constexpr unsigned cross=1u<<6, down=1u<<14;
        constexpr unsigned keys[]={down,cross,cross,cross,down,down,down,down,down,down,down,cross};
        const auto f=menu_test_frame++;
        if(f>=240){
            const auto step=(f-240)/36;
            const auto confirm=ManhattanTurnTrial()?5u:11u;
            if(step>confirm)menu_test_frame=0;
            else if((f-240)%36<3)sample.buttons=step==confirm?cross:keys[step];
        }
    }
    if (route_active && (sample.buttons & (1u << 11))) StopPerformanceRoute();
    if (route_active && route_step < PerformanceRouteLength && sample.connected) {
        // Repeat a logical input sequence on the original physics clock.
        // Apply ordinary skating, ollies and both directions of camera input.
        const unsigned leg = route_step / 180;
        const unsigned within = route_step % 180;
        sample.buttons = within < 145 ? 1u << 6 : 0;
        sample.left_x = (leg % 2 ? 1 : -1) * (within >= 70 && within < 114 ? 92 : 0);
        sample.left_y = 0;
        sample.right_x = (leg % 2 ? -1 : 1) * (within >= 114 && within < 157 ? 100 : 0);
        sample.right_y = 0;
        if(MoscowTurnTrial()){
            sample.buttons=0; sample.left_x=sample.left_y=sample.right_x=sample.right_y=0;
        }
        ++route_step;
    }
    connected = sample.connected != 0;
    EncodeInputPacket(sample, packet);
}
void CloseInput() {
    controller(); Dbg_Assert(opened);
    if (source.owner) source.close(source.owner); else source_input_close();
    opened = false; connected = false;
    StopPerformanceRoute();
    EncodeInputPacket({}, packet);
}
bool InputConnected() { controller(); Dbg_Assert(opened); return connected; }
void CopyInputPacket(unsigned char* out) {
    controller(); Dbg_Assert(opened && out);
    std::memcpy(out, packet, sizeof(packet));
}
}
