#include <source_port/input_os.h>
#include <source_port/application_os.h>
#include <errno.h>
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
static int opened;
static int external_controls;
int source_input_open(void) {
    if (opened) { errno = EALREADY; return -1; }
    Result result = hidInit();
    if (R_FAILED(result)) { errno = EIO; return -1; }
    external_controls=source_application_os_is_new_3ds();
    if(external_controls){
        result=irrstInit();
        if(R_FAILED(result)){hidExit();errno=ENODEV;return -1;}
    }
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
