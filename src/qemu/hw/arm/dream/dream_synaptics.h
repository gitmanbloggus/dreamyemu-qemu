#ifndef HW_ARM_DREAM_SYNAPTICS_H
#define HW_ARM_DREAM_SYNAPTICS_H

#include "hw/core/qdev.h"

#define TYPE_DREAM_SYNAPTICS "dream-synaptics"

/* a finger at screen pixel (x, y), or lifted */
void dream_synaptics_touch(DeviceState *dev, bool down, int x, int y);

#endif
