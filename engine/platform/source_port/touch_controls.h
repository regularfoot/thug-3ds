#pragma once
#include <source_port/input_os.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { SOURCE_TOUCH_WIDTH = 320, SOURCE_TOUCH_HEIGHT = 240, SOURCE_TOUCH_ZONES = 6 };
typedef struct SourceTouchZone {
    int x, y, width, height;
    uint16_t buttons;
    const char* label;
} SourceTouchZone;
extern const SourceTouchZone source_touch_zones[SOURCE_TOUCH_ZONES];

typedef struct SourceTouchGesture {
    const SourceTouchZone* zone;
    int origin_x, origin_y;
} SourceTouchGesture;

// Merge into this poll's fresh physical sample. A contact keeps its original
// control until lifted, even if a camera drag crosses a button.
void source_input_apply_touch(SourceTouchGesture* gesture, int held, int x, int y,
                              SourceInputSample* sample);
#ifdef __cplusplus
}
#endif
