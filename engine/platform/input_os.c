#include <source_port/input_os.h>
#include <source_port/application_os.h>
#include <source_port/touch_controls.h>
#include <errno.h>
#include <string.h>

enum { pad_l2 = 1u << 0, pad_r2 = 1u << 1, pad_l3 = 1u << 9, pad_r3 = 1u << 10 };
const SourceTouchZone source_touch_zones[SOURCE_TOUCH_ZONES] = {
    {  0,   0, 104,  52, pad_l2,          "ZL"},
    {104,   0, 112,  52, pad_l2 | pad_r2, "ZL+ZR"},
    {216,   0, 104,  52, pad_r2,          "ZR"},
    {  0,  52, 320, 140, 0,               "DRAG TO LOOK"},
    {  0, 192, 160,  48, pad_l3,          "L3"},
    {160, 192, 160,  48, pad_r3,          "R3"}
};

void source_input_apply_touch(SourceTouchGesture* gesture, int held, int x, int y,
                              SourceInputSample* sample) {
    sample->right_x = sample->right_y = 0;
    if (!held) {
        gesture->zone = NULL;
        return;
    }
    if (x < 0 || x >= SOURCE_TOUCH_WIDTH || y < 0 || y >= SOURCE_TOUCH_HEIGHT) return;
    if (!gesture->zone) {
        for (unsigned i = 0; i < SOURCE_TOUCH_ZONES; ++i) {
            const SourceTouchZone* zone = &source_touch_zones[i];
            if (x >= zone->x && x < zone->x + zone->width &&
                y >= zone->y && y < zone->y + zone->height) {
                gesture->zone = zone;
                gesture->origin_x = x;
                gesture->origin_y = y;
                break;
            }
        }
    }
    if (!gesture->zone) return;
    sample->buttons |= gesture->zone->buttons;
    if (!gesture->zone->buttons) {
        // The shared stick encoder clamps at full tilt after 39 pixels and
        // the original input manager applies its normal camera dead zone.
        sample->right_x = (x - gesture->origin_x) * 4;
        sample->right_y = (gesture->origin_y - y) * 4;
    }
}
#ifdef __3DS__
#include <3ds.h>
static int opened;
static int external_controls;
static SourceTouchGesture touch_gesture;
int source_input_open(void) {
    if (opened) { errno = EALREADY; return -1; }
    Result result = hidInit();
    if (R_FAILED(result)) { errno = EIO; return -1; }
    external_controls=source_application_os_is_new_3ds();
    if(external_controls){
        result=irrstInit();
        if(R_FAILED(result)){hidExit();errno=ENODEV;return -1;}
    }
    touch_gesture = (SourceTouchGesture){0};
    opened = 1;
    return 0;
}
int source_input_read(SourceInputSample* s) {
    if (!opened || !s) { errno = EINVAL; return -1; }
    hidScanInput(); // libctru also scans IRRST; never advance input per stereo eye.
    u32 keys = hidKeysHeld();
    circlePosition left, right={0};
    hidCircleRead(&left);
    if(external_controls)irrstCstickRead(&right);
    memset(s, 0, sizeof(*s));
    s->connected = 1;
    s->left_x = left.dx; s->left_y = left.dy;
    s->right_x = right.dx; s->right_y = right.dy;
    const u32 hardware[] = {KEY_ZL, KEY_ZR, KEY_L, KEY_R, KEY_X, KEY_A,
        KEY_B, KEY_Y, KEY_SELECT, 0, 0, KEY_START,
        KEY_DUP, KEY_DRIGHT, KEY_DDOWN, KEY_DLEFT};
    for (unsigned i = 0; i < 16; ++i)
        if (keys & hardware[i]) s->buttons |= (uint16_t)(1u << i);
    if (!external_controls) {
        touchPosition touch = {0};
        if (keys & KEY_TOUCH) hidTouchRead(&touch);
        source_input_apply_touch(&touch_gesture, keys & KEY_TOUCH, touch.px, touch.py, s);
    }
    return 0;
}
void source_input_close(void) {
    if (opened) { if(external_controls)irrstExit(); hidExit(); opened = 0; }
}
#else
int source_input_open(void) { errno = ENOTSUP; return -1; }
int source_input_read(SourceInputSample* s) { (void)s; errno = ENOTSUP; return -1; }
void source_input_close(void) {}
#endif
