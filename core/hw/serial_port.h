/*
	A serial port and what is plugged into it.

	A device on a serial line (a card reader, a touch panel, a hopper) is a
	Pipe: the port hands it each byte the game sends and asks it for the
	bytes it has to send back. The ports are the SH4's SCIF
	(hw/sh4/modules/serial.cpp) and the RS-422 line of a NAOMI's I/O board
	bridge (hw/maple/maple_devs.cpp).

	Everything here runs on the thread that runs the machine.
*/
#pragma once
#include "types.h"

class SerialPort
{
public:
	class Pipe
	{
	public:
		// a byte the game sends
		virtual void write(u8 data) { }
		virtual void sendBreak() { }
		// how many bytes the device has for the game
		virtual int available() { return 0; }
		// the next of them (only asked when there is one)
		virtual u8 read() { return 0; }

		virtual ~Pipe() = default;
	};

	virtual void setPipe(Pipe *pipe) = 0;
	// The device has something new to send: the port is to look for it.
	virtual void updateStatus() = 0;
	virtual void receiveBreak() { }

	virtual ~SerialPort() = default;
};
