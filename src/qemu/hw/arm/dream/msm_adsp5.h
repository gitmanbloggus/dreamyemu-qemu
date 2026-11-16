#ifndef HW_ARM_DREAM_MSM_ADSP5_H
#define HW_ARM_DREAM_MSM_ADSP5_H

#include "hw/core/qdev.h"

#define TYPE_MSM_ADSP5 "msm-adsp5"

/* AMSS 6225 qdsp_module_type ids (arch/arm/mach-msm/qdsp5/adsp_6225.c) */
#define ADSP5_MOD_AUDPLAY0TASK  2
#define ADSP5_MOD_AUDPPTASK     4
#define ADSP5_MOD_VIDEOTASK     5

/*
 * The modem's adsp_rtos RPC server (0x3000000a) enabled or disabled a task
 * module; the DSP drops that task's state.
 */
void msm_adsp5_module_enable(void *adsp, uint32_t module, bool enable);

#endif
