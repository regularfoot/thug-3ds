#pragma once

// Citro3D uses a signed interocular distance for each tilted projection.
// At 90 degrees horizontal FOV, geometry at this depth has zero disparity.
#define SOURCE_STEREO_FOCUS 1000.0f
#define SOURCE_STEREO_MAX_IOD 6.0f

// Azahar permits travel beyond the hardware endpoints. Keep the same
// projection and culling envelope on either device; NaN stays invalid.
static inline float source_stereo_slider(float raw)
{
    if (raw < 0.0f) return 0.0f;
    if (raw > 1.0f) return 1.0f;
    return raw;
}

static inline float source_stereo_iod(float slider)
{
    return SOURCE_STEREO_MAX_IOD * slider;
}

static inline float source_stereo_eye_iod(float slider, unsigned eye)
{
    const float iod = source_stereo_iod(slider);
    return eye == 0 ? -iod : iod;
}
