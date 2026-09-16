#include "emaster/observation/frame.h"

#include <string.h>

void emaster_observation_frame_clear(emaster_observation_frame_t *frame, uint16_t axis_count)
{
    if (frame == NULL)
    {
        return;
    }
    memset(frame, 0, sizeof(*frame));
    if (axis_count > (uint16_t)EMASTER_OBSERVATION_MAX_AXES)
    {
        axis_count = (uint16_t)EMASTER_OBSERVATION_MAX_AXES;
    }
    frame->axis_count = axis_count;
}
