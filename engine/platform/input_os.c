#include <source_port/input_os.h>
#include <source_port/application_os.h>
#include <source_port/touch_controls.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

enum { pad_l2 = 1u << 0, pad_r2 = 1u << 1, pad_l3 = 1u << 9, pad_r3 = 1u << 10 };
const SourceTouchZone source_touch_zones[SOURCE_TOUCH_ZONES] = {
    {  0,   0, 104,  52, pad_l2,          "ZL"},
    {104,   0, 112,  52, pad_l2 | pad_r2, "ZL+ZR"},
    {216,   0, 104,  52, pad_r2,          "ZR"},
    {  0,  52, 320, 140, 0,               "DRAG TO LOOK"},
    {  0, 192, 160,  48, pad_l3,          "L3"},
    {160, 192, 100,  48, pad_r3,          "R3"},
    {260, 192,  60,  48, 0,               "CFG"}
};

static const uint8_t default_buttons[SOURCE_PHYSICAL_BUTTONS] =
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15};

void source_input_default_map(SourceButtonMap* map) {
    memcpy(map->logical, default_buttons, sizeof(default_buttons));
}

static int valid_map(const SourceButtonMap* map) {
    unsigned used = 0;
    for (unsigned i = 0; i < SOURCE_PHYSICAL_BUTTONS; ++i) {
        unsigned value = map->logical[i];
        if (value >= SOURCE_LOGICAL_BUTTONS || (used & (1u << value))) return 0;
        used |= 1u << value;
    }
    return 1;
}

int source_input_assign_button(SourceButtonMap* map, unsigned physical, unsigned logical) {
    if (!map || !valid_map(map) || physical >= SOURCE_PHYSICAL_BUTTONS || logical >= SOURCE_LOGICAL_BUTTONS)
        return -1;
    for (unsigned i = 0; i < SOURCE_PHYSICAL_BUTTONS; ++i) {
        if (map->logical[i] == logical) {
            map->logical[i] = map->logical[physical];
            break;
        }
    }
    map->logical[physical] = (uint8_t)logical;
    return 0;
}

uint16_t source_input_map_buttons(const SourceButtonMap* map, uint16_t physical) {
    uint16_t result = 0;
    for (unsigned i = 0; i < SOURCE_PHYSICAL_BUTTONS; ++i)
        if (physical & (1u << i)) result |= (uint16_t)(1u << map->logical[i]);
    return result;
}

int source_input_load_map(const char* path, SourceButtonMap* map) {
    FILE* file = fopen(path, "rb");
    if (!file) return -1;
    char magic[5] = {0};
    SourceButtonMap candidate;
    int ok = fread(magic, 1, 4, file) == 4 && !memcmp(magic, "TC01", 4) &&
             fread(candidate.logical, 1, sizeof(candidate.logical), file) == sizeof(candidate.logical) &&
             fgetc(file) == EOF && !ferror(file) && valid_map(&candidate);
    fclose(file);
    if (!ok) { errno = EINVAL; return -1; }
    *map = candidate;
    return 0;
}

int source_input_save_map(const char* path, const SourceButtonMap* map) {
    if (!path || !map || !valid_map(map)) { errno = EINVAL; return -1; }
    char temporary[1024];
    int count = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (count < 0 || (size_t)count >= sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    FILE* file = fopen(temporary, "wb");
    if (!file) return -1;
    int ok = fwrite("TC01", 1, 4, file) == 4 &&
             fwrite(map->logical, 1, sizeof(map->logical), file) == sizeof(map->logical);
    if (fclose(file)) ok = 0;
    if (ok && !rename(temporary, path)) return 0;
    remove(temporary);
    return -1;
}

static char controls_path[1024] = "sdmc:/3ds/thug1-controls.cfg";
void source_input_set_settings_root(const char* app_root) {
    if (!app_root) return;
    const char* slash = strrchr(app_root, '/');
    if (!slash) return;
    const size_t parent = (size_t)(slash - app_root);
    if (parent + sizeof("/thug1-controls.cfg") > sizeof(controls_path)) return;
    memcpy(controls_path, app_root, parent);
    strcpy(controls_path + parent, "/thug1-controls.cfg");
}

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
            if (i == SOURCE_TOUCH_ZONES - 1) continue; // configuration button is handled on press
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
#include <source_port/graphics_os.h>
static int opened;
static int external_controls;
static SourceTouchGesture touch_gesture;
static SourceButtonMap button_map, pending_map;
static int settings_open, settings_waiting, settings_selected, settings_release;
static char settings_message[40];
static const unsigned physical_keys[SOURCE_PHYSICAL_BUTTONS] = {
    KEY_ZL, KEY_ZR, KEY_L, KEY_R, KEY_X, KEY_A, KEY_B, KEY_Y,
    KEY_SELECT, KEY_START, KEY_DUP, KEY_DRIGHT, KEY_DDOWN, KEY_DLEFT
};
static const char* physical_names[SOURCE_PHYSICAL_BUTTONS] = {
    "ZL", "ZR", "L", "R", "X", "A", "B", "Y", "SELECT", "START",
    "UP", "RIGHT", "DOWN", "LEFT"
};
static const char* logical_names[SOURCE_LOGICAL_BUTTONS] = {
    "L2", "R2", "L1", "R1", "GRIND", "GRAB", "JUMP", "FLIP",
    "SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT"
};

static void draw_settings(void) {
    char text[1100];
    size_t used = (size_t)snprintf(text, sizeof(text),
        "CONTROL SETTINGS\nD-PAD: select  A: change\nSTART: save  B: cancel\nSELECT: defaults\n\n");
    for (unsigned logical = 0; logical < SOURCE_LOGICAL_BUTTONS && used < sizeof(text); ++logical) {
        const char* physical = "--";
        for (unsigned i = 0; i < SOURCE_PHYSICAL_BUTTONS; ++i)
            if (pending_map.logical[i] == logical) physical = physical_names[i];
        int n = snprintf(text + used, sizeof(text) - used, "%c %-9s %s\n",
            settings_selected == (int)logical ? '>' : ' ', logical_names[logical], physical);
        if (n < 0 || (size_t)n >= sizeof(text) - used) break;
        used += (size_t)n;
    }
    snprintf(text + used, sizeof(text) - used, "\n%s",
        settings_waiting ? "Press a button. Tap lower-right to cancel." : settings_message);
    source_graphics_controls_text(text);
}

static int settings_hit(touchPosition touch) {
    return touch.px >= 260 && touch.px < 320 && touch.py >= 192 && touch.py < 240;
}

static void update_settings(u32 down, touchPosition touch) {
    if ((down & KEY_TOUCH) && settings_hit(touch)) {
        if (settings_open && settings_waiting) {
            settings_waiting = 0;
        } else if (!settings_open) {
            pending_map = button_map;
            settings_open = 1;
            settings_selected = 0;
            settings_message[0] = 0;
            touch_gesture = (SourceTouchGesture){0};
        }
        draw_settings();
        return;
    }
    if (!settings_open) return;
    if (settings_waiting) {
        for (unsigned i = 0; i < SOURCE_PHYSICAL_BUTTONS; ++i) {
            if (!(down & physical_keys[i])) continue;
            source_input_assign_button(&pending_map, i, (unsigned)settings_selected);
            settings_waiting = 0;
            settings_message[0] = 0;
            draw_settings();
            return;
        }
    } else if (down & KEY_DUP) {
        settings_selected = (settings_selected + SOURCE_LOGICAL_BUTTONS - 1) % SOURCE_LOGICAL_BUTTONS;
    } else if (down & KEY_DDOWN) {
        settings_selected = (settings_selected + 1) % SOURCE_LOGICAL_BUTTONS;
    } else if (down & KEY_A) {
        settings_waiting = 1;
    } else if (down & KEY_SELECT) {
        source_input_default_map(&pending_map);
        snprintf(settings_message, sizeof(settings_message), "Defaults restored.");
    } else if (down & KEY_START) {
        if (source_input_save_map(controls_path, &pending_map)) {
            snprintf(settings_message, sizeof(settings_message), "Save failed. Try again.");
        } else {
            button_map = pending_map;
            settings_open = 0;
            settings_release = 1;
            source_graphics_controls_text("");
            return;
        }
    } else if (down & KEY_B) {
        settings_open = 0;
        settings_release = 1;
        source_graphics_controls_text("");
        return;
    } else {
        return;
    }
    draw_settings();
}
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
    source_input_default_map(&button_map);
    source_input_load_map(controls_path, &button_map);
    settings_open = settings_waiting = settings_release = 0;
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
    const u32 down = hidKeysDown();
    touchPosition touch = {0};
    if (keys & KEY_TOUCH) hidTouchRead(&touch);
    update_settings(down, touch);
    if (settings_open || settings_release) {
        if (settings_release && !(keys & (KEY_TOUCH | KEY_A | KEY_B | KEY_START)))
            settings_release = 0;
        s->left_x = s->left_y = s->right_x = s->right_y = 0;
        return 0;
    }
    uint16_t physical = 0;
    for (unsigned i = 0; i < SOURCE_PHYSICAL_BUTTONS; ++i)
        if (keys & physical_keys[i]) physical |= (uint16_t)(1u << i);
    s->buttons = source_input_map_buttons(&button_map, physical);
    if (!external_controls)
        source_input_apply_touch(&touch_gesture, keys & KEY_TOUCH, touch.px, touch.py, s);
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
