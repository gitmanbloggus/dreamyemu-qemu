#ifndef HW_ARM_DREAM_INPUT_H
#define HW_ARM_DREAM_INPUT_H

#include "../htcleo/htcleo.h"

/* keypad matrix, power key and CPLD key bank; boot_keys e.g. "power+back" */
void dream_input_init(MsmGpioState *gpio, const char *boot_keys,
                      DeviceState *touchscreen);

/* the microSD slot's card switch (CPLD GPIO 176): level 1 = no card */
qemu_irq dream_input_sd_detect(void);

#endif
