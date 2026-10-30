#ifndef HW_ARM_DREAM_MSM_MDDI_H
#define HW_ARM_DREAM_MSM_MDDI_H

#define TYPE_MSM_MDDI "msm-mddi"

#include "hw/core/qdev.h"

/* The MDP's DMA2 output to the panel: RGB565 rows from guest memory */
void msm_mddi_panel_blit(DeviceState *mddi, uint32_t base, uint32_t stride,
                         int w, int h, int x, int y);

#endif
