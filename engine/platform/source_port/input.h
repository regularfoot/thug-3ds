#pragma once
#include <source_port/input_os.h>
namespace SourcePort {
// Explicit providers support tests/devices; production defaults to real HID.
struct InputSource {
    void* owner;
    bool (*open)(void*);
    bool (*read)(void*, SourceInputSample*);
    void (*close)(void*);
};
void BindInputSource(InputSource source);
void UnbindInputSource(void* owner);
void OpenInput();
void RefreshInput();
void CloseInput();
bool InputConnected();
void CopyInputPacket(unsigned char* packet);
void EncodeInputPacket(const SourceInputSample& sample, unsigned char* packet);
// Controlled Free Skate route. Start cancels and returns to ordinary input.
constexpr unsigned PerformanceRouteLength = 1080;
void BeginCityMenuTrial();
void StartPerformanceRoute();
void StopPerformanceRoute();
unsigned PerformanceRouteStep();
}
