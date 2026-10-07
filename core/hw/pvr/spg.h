#pragma once
#include "types.h"

bool spg_Init();
void spg_Term();
void spg_Reset(bool Manual);

void CalculateSync(bool reschedule = true);
double spg_get_refresh_rate();
void read_lightgun_position(int x, int y);
// Before a read of SPG_STATUS: brings its scanline up to where the beam is
void spg_sync();
// A write to SPG_HBLANK_INT, SPG_VBLANK_INT or SPG_VBLANK
void spg_write_timing(u32 addr, u32 data);
struct TA_context;
void SetREP(TA_context* cntx);
