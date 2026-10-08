/* The NAOMI 2's geometry processor in the machine. See elan_host.cpp. */
#pragma once
#include "types.h"

extern u8 *elan_ram;       // the chip's 32 MB, when the machine is a NAOMI 2
extern int elan_schid;     // the end of a texture transfer

void elan_host_init();
void elan_host_term();
void elan_host_reset(bool hard);
void elan_host_map_init();
void elan_host_map(u32 base);
