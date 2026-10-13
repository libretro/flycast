#pragma once
#include "types.h"

void bsc_init();
void bsc_reset(bool hard);
void bsc_term();


void cpg_init();
void cpg_reset();
void cpg_term();

void dmac_init();
void dmac_reset();
void dmac_term();

void rtc_init();
void rtc_reset();
void rtc_term();

void intc_init();
void intc_reset();
void intc_term();

void serial_init();
void serial_reset();
void serial_term();

#include "hw/serial_port.h"
/* The device plugged into the SCIF, if any (serial.cpp): NULL unplugs it.
 * scif_update_status() is for a device that has something new to send. */
void scif_set_pipe(SerialPort::Pipe *pipe);
SerialPort::Pipe *scif_get_pipe();
void scif_update_status();
// what a device's port has in a state since V24: see serial.cpp
void scif_serialize_v24(void **data, unsigned int *total_size);
bool scif_unserialize_v24(void **data, unsigned int *total_size);
void scif_state_before_v24(void);

// The same, the way the peripherals' code has it
class SCIFSerialPort : public SerialPort
{
public:
	void setPipe(Pipe *pipe) override;
	Pipe *getPipe() const { return scif_get_pipe(); }
	void updateStatus() override;
	static SCIFSerialPort& Instance();
};

void ubc_init();
void ubc_reset();
void ubc_term();

void tmu_init();
void tmu_reset(bool hard);
void tmu_term();
// after a state has been loaded: see tmu.cpp
void tmu_state_loaded(void);

void ccn_init();
void ccn_reset(bool hard);
void ccn_term();

void MMU_init();
void MMU_reset();
void MMU_term();
