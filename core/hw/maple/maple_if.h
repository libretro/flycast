#pragma once
#include "maple_devs.h"

extern maple_device* MapleDevices[4][6];

void maple_Init();
void maple_Reset(bool Manual);
void maple_Term();
void maple_ReconnectDevices();

void maple_vblank();

// The answers of the transfer under way, kept until it is over: see maple_if.cpp
#define MAPLE_OUT_WORDS 4096
extern u32 maple_out[MAPLE_OUT_WORDS];
extern u32 maple_out_used;
// The transfer is over
void maple_dma_done();
/* Flush pending VMU and EEPROM writes; the machine must be idle. */
void maple_FlushSaves();
