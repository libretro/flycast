#pragma once
#include "types.h"

/* The beeper of the Visual Memory units (the clock function's alarm). */

/* Set by the core option: whether the beep is heard. */
extern bool vmu_beep_enabled;

/* A game's alarm setting: period and on time, in the VMU's timer counts. */
void vmu_beep_set(u32 period, u32 on);
/* Silence, at once (a device reset, a reset of the machine). */
void vmu_beep_reset(void);
/* Adds the beep to samples about to go to the frontend. */
void vmu_beep_mix(SoundFrame *frames, u32 count);

void vmu_beep_serialize_v24(void **data, unsigned int *total_size);
bool vmu_beep_unserialize_v24(void **data, unsigned int *total_size);
void vmu_beep_state_before_v24(void);
