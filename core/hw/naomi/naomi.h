/*
**	naomi.h
*/

#pragma once

void naomi_reg_Init();
void naomi_reg_Term();
void naomi_reg_Reset(bool Manual);

u32  ReadMem_naomi(u32 Addr, u32 size);
void WriteMem_naomi(u32 Addr, u32 data, u32 size);

void NaomiBoardIDWrite(u16 Data);
u16 NaomiBoardIDRead();
u16 NaomiGameIDRead();
void NaomiGameIDWrite(u16 Data);
// the serial EEPROMs of the main board and the ROM board (naomi.cpp)
void naomi_serial_id_load(const u8 *rom_board);
const u8 *naomi_rom_serial_data(void);
void naomi_rom_serial_reset(void);
void naomi_rom_serial_serialize(void **data, unsigned int *total_size);
void naomi_rom_serial_unserialize(void **data, unsigned int *total_size);
void naomi_serial_serialize_v26(void **data, unsigned int *total_size);
bool naomi_serial_unserialize_v26(void **data, unsigned int *total_size);
void naomi_serial_state_before_v26(void);
void naomi_process(u32 command,u32 offset1,u32 parameterl, u32 parameterh);

extern u32 reg_dimm_command;	// command, written, 0x1E03 some flag ?
extern u32 reg_dimm_offsetl;
extern u32 reg_dimm_parameterl;
extern u32 reg_dimm_parameterh;
extern u32 reg_dimm_status;
