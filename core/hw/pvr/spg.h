#pragma once
#include "types.h"

bool spg_Init();
void spg_Term();
void spg_Reset(bool Manual);

void CalculateSync();
double spg_get_refresh_rate();
void read_lightgun_position(int x, int y);
struct TA_context;
void SetREP(TA_context* cntx);
