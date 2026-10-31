#ifndef HW_ARM_DREAM_INPUT_H
#define HW_ARM_DREAM_INPUT_H

#include "../htcleo/htcleo.h"

/* keypad matrix, power key and CPLD key bank; boot_keys e.g. "power+back" */
void dream_input_init(MsmGpioState *gpio, const char *boot_keys,
                      DeviceState *touchscreen);
/* the keyboard slide (SW_LID): F11 toggles it, or set it directly */
bool dream_input_keyboard_open(void);
void dream_input_set_keyboard_open(bool open);

#endif
