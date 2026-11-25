#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Logical PS2 button bit positions, consumed unchanged by original Inp. */
typedef struct SourceInputSample {
    uint16_t buttons;
    int16_t left_x, left_y, right_x, right_y;
    int connected;
} SourceInputSample;
enum { SOURCE_PHYSICAL_BUTTONS = 14, SOURCE_LOGICAL_BUTTONS = 16 };
typedef struct SourceButtonMap {
    uint8_t logical[SOURCE_PHYSICAL_BUTTONS];
} SourceButtonMap;
void source_input_default_map(SourceButtonMap* map);
int source_input_assign_button(SourceButtonMap* map, unsigned physical, unsigned logical);
uint16_t source_input_map_buttons(const SourceButtonMap* map, uint16_t physical);
int source_input_load_map(const char* path, SourceButtonMap* map);
int source_input_save_map(const char* path, const SourceButtonMap* map);
void source_input_set_settings_root(const char* app_root);
int source_input_open(void);
int source_input_read(SourceInputSample* sample);
void source_input_close(void);
#ifdef __cplusplus
}
#endif
