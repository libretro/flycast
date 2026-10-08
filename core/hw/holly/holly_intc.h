#pragma once
#include "types.h"

void asic_RaiseInterrupt(HollyInterruptID inter);
void asic_CancelInterrupt(HollyInterruptID inter);

// NAOMI 2: the second PowerVR
extern u32 SB_ISTNRM1;
void asic_RaiseInterruptBothCLX(HollyInterruptID inter);
bool asic_IsCLXB(u32 addr);
u32 asic_ReadCLXB(u32 addr);
void asic_WriteCLXB(u32 addr, u32 data);

//Init/Res/Term for regs
void asic_reg_Init();
void asic_reg_Term();
void asic_reg_Reset(bool Manual);
