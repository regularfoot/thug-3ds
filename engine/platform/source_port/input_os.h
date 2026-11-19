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
int source_input_open(void);
int source_input_read(SourceInputSample* sample);
void source_input_close(void);
#ifdef __cplusplus
}
#endif
