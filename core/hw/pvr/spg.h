#pragma once
#include "types.h"

bool spg_Init();
void spg_Term();
void spg_Reset(bool Manual);

void CalculateSync(bool reschedule = true);
double spg_get_refresh_rate();
void read_lightgun_position(int x, int y);
// The game cancelled the gun's occupation of the bus: no end owed at vblank out
void spg_gun_occupy_cancel();
// Before a read of SPG_STATUS: brings its scanline up to where the beam is
void spg_sync();
// A write to SPG_HBLANK_INT, SPG_VBLANK_INT or SPG_VBLANK
void spg_write_timing(u32 addr, u32 data);
// Where a run (a retro_run's frame) ends, spg.cpp: a run starts (dc_run())...
void spg_frame_begin();
// ...a render to the screen has been started...
void spg_screen_render();
// ...and a maple transfer: true if it waits for the start of the next run
bool spg_maple_defer();
// dc_run()'s backstop has stopped the run where it is
void spg_frame_cut();
// A transfer that asked a light gun where it points is under way
bool spg_gun_pending();
struct TA_context;
void SetREP(TA_context* cntx);
