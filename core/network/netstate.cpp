/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "netservice.h"
#ifdef ENABLE_MODEM
#include "hw/modem/modem.h"
#include "hw/bba/bba.h"
#include "network/picoppp.h"
#endif

u8 net_device = NET_DEVICE_NONE;
u8 net_device_option = NET_DEVICE_MODEM;
bool net_dcnet_option = false;

/* The device's part of a save state is always this long on a Dreamcast,
 * whichever device it is and whichever build made it (one without the
 * network devices writes zeroes): the broadband adapter's, which is the
 * longer, and the modem's after it in the same room. */
#define NET_V24_PAYLOAD 34473

static u8 device_for_machine(u8 dev)
{
#ifdef ENABLE_MODEM
	if (settings.System != DC_PLATFORM_DREAMCAST || dev > NET_DEVICE_BBA)
		return NET_DEVICE_NONE;
	return dev;
#else
	(void)dev;
	return NET_DEVICE_NONE;
#endif
}

/* The machine is given @dev in place of the device it has. When a state is
 * being loaded, the modem's registers have been read from it already and
 * are left as they are. */
static void switch_device(u8 dev, bool loading)
{
	dev = device_for_machine(dev);
	if (dev == net_device)
		return;
#ifdef ENABLE_MODEM
	INFO_LOG(MODEM, "Expansion port device %d -> %d", net_device, dev);
	if (net_device == NET_DEVICE_BBA)
		bba_Term();
	else if (net_device == NET_DEVICE_MODEM && !loading)
		ModemTerm();
	net_device = dev;
	if (dev == NET_DEVICE_BBA)
		bba_Init();
#else
	(void)loading;
	net_device = dev;
#endif
}

void net_device_attach(void)
{
	net_device = NET_DEVICE_NONE;
	switch_device(net_device_option, false);
}

void net_device_reset(bool hard)
{
	if (settings.System != DC_PLATFORM_DREAMCAST)
		return;
	// The option's device, if it is not this one, is put in at a hard reset
	if (hard && device_for_machine(net_device_option) != net_device)
		switch_device(net_device_option, false);
#ifdef ENABLE_MODEM
	if (net_device == NET_DEVICE_BBA)
		bba_Reset(hard);
	else
		ModemTerm();
#endif
}

void net_device_detach(void)
{
#ifdef ENABLE_MODEM
	if (net_device == NET_DEVICE_BBA)
		bba_Term();
	else
		ModemTerm();
	// No thread of this core's is left running
	net_thread_join();
#endif
	net_device = NET_DEVICE_NONE;
}

// Zeroes, @size bytes
static void write_zeroes(void **data, unsigned int *total_size, unsigned size)
{
	static const u8 zeroes[1024] = { 0 };
	while (size > 0)
	{
		const unsigned n = size < sizeof(zeroes) ? size : sizeof(zeroes);
		LIBRETRO_SA(zeroes, n);
		size -= n;
	}
}

static void skip(void **data, unsigned int *total_size, unsigned size)
{
	u8 scratch[1024];
	while (size > 0)
	{
		const unsigned n = size < sizeof(scratch) ? size : sizeof(scratch);
		LIBRETRO_USA(scratch, n);
		size -= n;
	}
}

/* Only a Dreamcast has an expansion port, and nothing is written for the
 * others: a byte for the device, then the device's part. */
void net_serialize_v24(void **data, unsigned int *total_size)
{
	if (settings.System != DC_PLATFORM_DREAMCAST)
		return;
	u8 dev = net_device;
	LIBRETRO_S(dev);
	const unsigned start = *total_size;
#ifdef ENABLE_MODEM
	if (dev == NET_DEVICE_MODEM)
		modem_serialize_v24(data, total_size);
	else if (dev == NET_DEVICE_BBA)
		bba_serialize_v24(data, total_size);
#endif
	const unsigned used = *total_size - start;
	if (used > NET_V24_PAYLOAD)
		ERROR_LOG(MODEM, "The network device's part of the state is %d bytes: no more than %d fit", used, NET_V24_PAYLOAD);
	else
		write_zeroes(data, total_size, NET_V24_PAYLOAD - used);
}

bool net_unserialize_v24(void **data, unsigned int *total_size)
{
	if (settings.System != DC_PLATFORM_DREAMCAST)
		return true;
	u8 dev = NET_DEVICE_NONE;
	LIBRETRO_US(dev);
	const unsigned start = *total_size;
	// The state's machine had this device: so does this one from now on
	// (a device this build has not, or not known, is no device)
	switch_device(dev, true);
	dev = net_device;
#ifdef ENABLE_MODEM
	if (dev == NET_DEVICE_MODEM)
		modem_unserialize_v24(data, total_size);
	else
	{
		modem_state_before_v24();
		if (dev == NET_DEVICE_BBA)
			bba_unserialize_v24(data, total_size);
	}
#endif
	const unsigned used = *total_size - start;
	if (used < NET_V24_PAYLOAD)
		skip(data, total_size, NET_V24_PAYLOAD - used);
	return true;
}

/* An older state is of a machine with a modem: a state made with the
 * broadband adapter cannot be loaded (see dc_unserialize()). */
void net_state_before_v24(void)
{
	if (settings.System != DC_PLATFORM_DREAMCAST)
		return;
	switch_device(NET_DEVICE_MODEM, true);
#ifdef ENABLE_MODEM
	if (net_device == NET_DEVICE_MODEM)
		modem_state_before_v24();
#endif
}
