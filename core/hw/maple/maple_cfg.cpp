#include "types.h"
#include "maple_if.h"
#include "maple_helper.h"
#include "maple_devs.h"
#include "maple_cfg.h"
#include "libretro/input_latch.h"

#include "lockfree.h"

static cTripleBuffer vmu_lcd_exchange[4];
static u8 vmu_lcd_buffers[4][3][VMU_SCREEN_WIDTH * VMU_SCREEN_HEIGHT];
/* 1 once a picture has been published, 2 once the renderer has one */
static retro_atomic_int_t vmu_lcd_state[4];

void vmu_lcd_publish(int screen, const u8 *pixels)
{
	if (screen < 0 || screen >= 4 || pixels == NULL)
		return;
	memcpy(vmu_lcd_buffers[screen][vmu_lcd_exchange[screen].Back()], pixels,
			VMU_SCREEN_WIDTH * VMU_SCREEN_HEIGHT);
	vmu_lcd_exchange[screen].Publish();
	if (retro_atomic_load_acquire_int(&vmu_lcd_state[screen]) == 0)
		retro_atomic_store_release_int(&vmu_lcd_state[screen], 1);
}

bool vmu_lcd_refresh(int screen)
{
	if (screen < 0 || screen >= 4 || retro_atomic_load_acquire_int(&vmu_lcd_state[screen]) == 0)
		return false;
	if (!vmu_lcd_exchange[screen].Take())
		return false;
	retro_atomic_store_release_int(&vmu_lcd_state[screen], 2);
	return true;
}

const u8 *vmu_lcd_pixels(int screen)
{
	if (screen < 0 || screen >= 4 || retro_atomic_load_acquire_int(&vmu_lcd_state[screen]) != 2)
		return NULL;
	return vmu_lcd_buffers[screen][vmu_lcd_exchange[screen].Front()];
}

void vmu_lcd_reset(void)
{
	for (int screen = 0; screen < 4; screen++)
	{
		retro_atomic_store_release_int(&vmu_lcd_state[screen], 0);
		vmu_lcd_exchange[screen].Reset();
	}
}

#define HAS_VMU
/*
bus_x=0{p0=1{config};p1=2{config};config;}
Plugins:
	Input Source
		EventMap -- 'Raw' interface, source_name[seid]:mode
		KeyMap -- translated chars ( no re-mapping possible)
	Output
		Image
		
*/
/*
	MapleConfig:
		InputUpdate(&fmt);
		ImageUpdate(data);
*/
void UpdateInputState(u32 port);
void UpdateInputStateArcade(void);
void UpdateInputStateMouse(u32 port);
extern InputLatch input_latch;
void UpdateVibration(u32 port, u32 value, u32 max_duration);

extern u32 kcode[4];
extern s8 joyx[4], joyy[4], joyrx[4], joyry[4];
extern u8 rt[4],lt[4];
extern bool enable_purupuru;
extern f32 mo_x_abs[4];
extern f32 mo_y_abs[4];
extern u32 mo_buttons[4];
extern f32 mo_x_delta[4];
extern f32 mo_y_delta[4];
extern f32 mo_wheel_delta[4];
extern bool EEPROM_loaded;

const char* VMU_SCREEN_COLOR_NAMES[VMU_NUM_COLORS] = {
		"DEFAULT_ON",
		"DEFAULT_OFF",
		"BLACK",
		"BLUE",
		"LIGHT_BLUE",
		"GREEN",
		"GREEN_BLUE",
		"GREEN_LIGHT_BLUE",
		"LIGHT_GREEN",
		"LIGHT_GREEN_BLUE",
		"LIGHT_GREEN_LIGHT_BLUE",
		"RED",
		"RED_BLUE",
		"RED_LIGHT_BLUE",
		"RED_GREEN",
		"RED_GREEN_BLUE",
		"RED_GREEN_LIGHT_BLUE",
		"RED_LIGHT_GREEN",
		"RED_LIGHT_GREEN_BLUE",
		"RED_LIGHT_GREEN_LIGHT_BLUE",
		"LIGHT_RED",
		"LIGHT_RED_BLUE",
		"LIGHT_RED_LIGHT_BLUE",
		"LIGHT_RED_GREEN",
		"LIGHT_RED_GREEN_BLUE",
		"LIGHT_RED_GREEN_LIGHT_BLUE",
		"LIGHT_RED_LIGHT_GREEN",
		"LIGHT_RED_LIGHT_GREEN_BLUE",
		"WHITE"
};
const rgb_t VMU_SCREEN_COLOR_MAP[VMU_NUM_COLORS] = {
		{ DEFAULT_VMU_PIXEL_ON_R,   DEFAULT_VMU_PIXEL_ON_G ,   DEFAULT_VMU_PIXEL_ON_B  },
		{ DEFAULT_VMU_PIXEL_OFF_R,  DEFAULT_VMU_PIXEL_OFF_G ,  DEFAULT_VMU_PIXEL_OFF_B  },
		{ 0x00, 0x00, 0x00 },
		{ 0x00, 0x00, 0x7F },
		{ 0x00, 0x00, 0xFF },
		{ 0x00, 0x7F, 0x00 },
		{ 0x00, 0x7F, 0x7F },
		{ 0x00, 0x7F, 0xFF },
		{ 0x00, 0xFF, 0x00 },
		{ 0x00, 0xFF, 0x7F },
		{ 0x00, 0xFF, 0xFF },
		{ 0x7F, 0x00, 0x00 },
		{ 0x7F, 0x00, 0x7F },
		{ 0x7F, 0x00, 0xFF },
		{ 0x7F, 0x7F, 0x00 },
		{ 0x7F, 0x7F, 0x7F },
		{ 0x7F, 0x7F, 0xFF },
		{ 0x7F, 0xFF, 0x00 },
		{ 0x7F, 0xFF, 0x7F },
		{ 0x7F, 0xFF, 0xFF },
		{ 0xFF, 0x00, 0x00 },
		{ 0xFF, 0x00, 0x7F },
		{ 0xFF, 0x00, 0xFF },
		{ 0xFF, 0x7F, 0x00 },
		{ 0xFF, 0x7F, 0x7F },
		{ 0xFF, 0x7F, 0xFF },
		{ 0xFF, 0xFF, 0x00 },
		{ 0xFF, 0xFF, 0x7F },
		{ 0xFF, 0xFF, 0xFF }
};

vmu_screen_params_t vmu_screen_params[4] ;

u8 lightgun_img_crosshair[LIGHTGUN_CROSSHAIR_SIZE*LIGHTGUN_CROSSHAIR_SIZE] =
{
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,
	1,1,1,1,1,1,0,0,0,0,1,1,1,1,1,1,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,
};

u8 lightgun_palette[LIGHTGUN_COLORS_COUNT*3] =
{
	0xff,0xff,0xff, // LIGHTGUN_COLOR_OFF
	0xff,0xff,0xff, // LIGHTGUN_COLOR_WHITE
	0xff,0x10,0x10, // LIGHTGUN_COLOR_RED
	0x10,0xff,0x10, // LIGHTGUN_COLOR_GREEN
	0x10,0x10,0xff, // LIGHTGUN_COLOR_BLUE
};

lightgun_params_t lightgun_params[4] ;

MapleDeviceType maple_devices[MAPLE_PORTS] =
   { MDT_SegaController, MDT_SegaController, MDT_SegaController, MDT_SegaController };

u8 GetBtFromSgn(s8 val)
{
	return val+128;
}

struct MapleConfigMap : IMapleConfigMap
{
	maple_device* dev;
	int player_num;

	MapleConfigMap(maple_device* dev, int player_num = -1)
	{
		this->dev=dev;
		this->player_num = player_num;
	}

   void SetVibration(u32 value, u32 max_duration)
   {
      UpdateVibration(player_num == -1 ? dev->bus_id : player_num, value, max_duration);
   }

	void GetInput(PlainJoystickState* pjs)
	{
	   int pnum = player_num == -1 ? dev->bus_id : player_num;
	   s8 x1, y1;

	   if (settings.rend.ThreadedRendering)
	   {
	      /* Sampled on the libretro thread by CaptureInput(). */
	      const InputLatch::Port& in = input_latch.Read(pnum);

	      pjs->kcode=in.kcode;
	      x1=in.joyx;
	      y1=in.joyy;
	      pjs->joy[PJAI_X2]=GetBtFromSgn(in.joyrx);
	      pjs->joy[PJAI_Y2]=GetBtFromSgn(in.joyry);
	      pjs->trigger[PJTI_R]=in.rt;
	      pjs->trigger[PJTI_L]=in.lt;
	   }
	   else
	   {
	      /* A NAOMI's I/O board is one device, on the first port, and
	       * answers for every player; an Atomiswave's coins are read for
	       * all four. An arcade machine's ports are read all together,
	       * once a frame, whichever device asks first. (A NAOMI's were all
	       * read each time a board was asked for its inputs, twice a frame
	       * where there are two boards.) */
	      if (settings.System == DC_PLATFORM_DREAMCAST)
	         UpdateInputState(pnum);
	      else
	         UpdateInputStateArcade();

	      pjs->kcode=kcode[pnum];
	      x1=joyx[pnum];
	      y1=joyy[pnum];
	      pjs->joy[PJAI_X2]=GetBtFromSgn(joyrx[pnum]);
	      pjs->joy[PJAI_Y2]=GetBtFromSgn(joyry[pnum]);
	      pjs->trigger[PJTI_R]=rt[pnum];
	      pjs->trigger[PJTI_L]=lt[pnum];
	   }
	   pjs->joy[PJAI_X1]=GetBtFromSgn(x1);
	   pjs->joy[PJAI_Y1]=GetBtFromSgn(y1);
	   // I think the only Atomiswave game that uses an analog joystick is Block Pong-Pong but only the x axis.
	   // Driving games use the first two axes for totally separate things.
	   // So only do this for dreamcast
	   if (settings.System == DC_PLATFORM_DREAMCAST)
	   	limit_joystick_magnitude<128>((s8&)pjs->joy[PJAI_X1], (s8&)pjs->joy[PJAI_Y1]);
	}
	void SetImage(void* img)
	{
		vmu_lcd_publish(player_num == -1 ? dev->bus_id : player_num, (const u8 *)img);
	}
	void GetAbsolutePosition(f32 *px, f32 *py)
	{
	   int pnum = player_num == -1 ? dev->bus_id : player_num;
	   *px = mo_x_abs[pnum];
	   *py = mo_y_abs[pnum];
	}
	void GetMouse(u32 *buttons, f32 *delta_x, f32 *delta_y, f32 *delta_wheel)
	{
	   int pnum = player_num == -1 ? dev->bus_id : player_num;

	   if (settings.rend.ThreadedRendering)
	   {
	      input_latch.ReadMouse(pnum, buttons, delta_x, delta_y, delta_wheel);
	      return;
	   }
	   if (settings.System == DC_PLATFORM_DREAMCAST)
	      UpdateInputStateMouse(pnum);
	   else
	      UpdateInputStateArcade();
	   *buttons = mo_buttons[pnum];
	   *delta_x = mo_x_delta[pnum];
	   *delta_y = mo_y_delta[pnum];
	   *delta_wheel = mo_wheel_delta[pnum];

	   mo_x_delta[pnum] = 0;
	   mo_y_delta[pnum] = 0;
	   mo_wheel_delta[pnum] = 0;
	}
};

void mcfg_Create(MapleDeviceType type,u32 bus,u32 port, int player_num /* = -1 */)
{
	maple_device* dev=maple_Create(type);
	dev->Setup(maple_GetAddress(bus,port));
	dev->config = new MapleConfigMap(dev, player_num);
	dev->OnSetup();
	MapleDevices[bus][port]=dev;
}

/* What a Dreamcast is to have on a bus, as the settings say: at port 5 the
 * device plugged in, at 0 and 1 what is in its slots. */
static MapleDeviceType mcfg_DreamcastDevice(int bus, int port)
{
	switch (maple_devices[bus])
	{
	case MDT_SegaController:
		if (port == 5)
			return MDT_SegaController;
		if (port == 0)
			return MDT_SegaVMU;
		if (port == 1)
			return enable_purupuru ? MDT_PurupuruPack : MDT_SegaVMU;
		break;

	case MDT_Keyboard:
	case MDT_Mouse:
		if (port == 5)
			return maple_devices[bus];
		break;

	case MDT_LightGun:
		if (port == 5)
			return MDT_LightGun;
		if (port == 0)
			return enable_purupuru ? MDT_PurupuruPack : MDT_SegaVMU;
		break;

	case MDT_TwinStick:
	case MDT_AsciiStick:
		if (port == 5)
			return maple_devices[bus];
		if (port == 0)
			return MDT_SegaVMU;
		break;

	default:
		break;
	}
	return MDT_None;
}

void mcfg_CreateDevices()
{
   int bus ;

   for ( bus = 0 ; bus < MAPLE_PORTS; bus++)
      vmu_screen_params[bus].vmu_lcd_screen = NULL ;
   vmu_lcd_reset();


   if (settings.System == DC_PLATFORM_DREAMCAST)
   {
	  for (bus = 0; bus < MAPLE_PORTS; ++bus)
	  {
		 if (maple_devices[bus] != MDT_None && mcfg_DreamcastDevice(bus, 5) == MDT_None)
			WARN_LOG(MAPLE, "Invalid device type %d for port %d", maple_devices[bus], bus);
		 if (mcfg_DreamcastDevice(bus, 5) == MDT_None)
			continue;
		 // the device, then what is in its slots
		 mcfg_Create(mcfg_DreamcastDevice(bus, 5), bus, 5);
		 for (int port = 0; port < 5; port++)
		 {
			const MapleDeviceType type = mcfg_DreamcastDevice(bus, port);
			if (type != MDT_None)
			   mcfg_Create(type, bus, port);
		 }
	  }
   }
   else if (SYSTEM_IS_NAOMI())
   {
      bus = 0;
      mcfg_Create(MDT_NaomiJamma, bus++, 5);
      for (int i = 0; i < MAPLE_PORTS; i++)
      {
      	switch (maple_devices[i])
      	{
      	case MDT_Keyboard:
      		mcfg_Create(MDT_Keyboard, bus++, 5);
      		break;
      	default:
      		if (i == 0)
      		{
      			// Connect VMU B1
      			mcfg_Create(MDT_SegaController, bus, 5);
      			mcfg_Create(MDT_SegaVMU, bus++, 0);
      		}
      		break;
      	}
      }
   }
   else if (settings.System == DC_PLATFORM_ATOMISWAVE)
   {
	  mcfg_Create(MDT_SegaController, 0, 5);
	  mcfg_Create(MDT_SegaController, 1, 5);
	  switch (settings.mapping.JammaSetup)
	  {
	  case JVS::Analog:
		 // 2 players with analog axes
		 mcfg_Create(MDT_SegaController, 2, 5, 0);
		 mcfg_Create(MDT_SegaController, 3, 5, 1);
		 break;
	  case JVS::LightGun:
		 // Light guns
		 mcfg_Create(MDT_LightGun, 2, 5, 0);
		 mcfg_Create(MDT_LightGun, 3, 5, 1);
		 break;
	  case JVS::RotaryEncoders:
	  case JVS::SegaMarineFishing:
		 // Track-ball
		 mcfg_Create(MDT_Mouse, 2, 5, 0);
		 break;
	  default:
		 // 4 players
		 mcfg_Create(MDT_SegaController, 2, 5);
		 mcfg_Create(MDT_SegaController, 3, 5);
		 break;
	  }
   }
}


void mcfg_DestroyDevices()
{
	for (int i = 0; i < MAPLE_PORTS; i++)
		for (int j=0;j<=5;j++)
		{
			delete MapleDevices[i][j];
			MapleDevices[i][j] = NULL;
		}
}

void mcfg_DestroyDevice(int i, int j)
{
	delete MapleDevices[i][j];
	MapleDevices[i][j] = NULL;
}

bool maple_vmu_file_changed(maple_device *dev);

/* A Dreamcast's devices made to be what the settings say, touching nothing
 * that already is: on a bus whose device is not the one wanted, the device
 * and all in its slots are unplugged; otherwise each slot whose device is
 * not the one wanted is, or whose card would now be read from another file.
 * With @plug_in, what is missing then is plugged in.
 *
 * Unplugging is what a person changing a controller does, and the game sees
 * the port empty and then the new device. The other ports go on as they
 * were: a game writing to a card in one of them while another port's device
 * is changed is not cut off half way through a save. */
void mcfg_ReconcileDevices(bool plug_in)
{
	for (int bus = 0; bus < MAPLE_PORTS; bus++)
	{
		maple_device *device = MapleDevices[bus][5];
		if (device == NULL || device->get_device_type() != mcfg_DreamcastDevice(bus, 5))
		{
			for (int port = 0; port <= 5; port++)
				if (MapleDevices[bus][port] != NULL)
					mcfg_DestroyDevice(bus, port);
		}
		else
		{
			for (int port = 0; port < 5; port++)
			{
				maple_device *dev = MapleDevices[bus][port];
				if (dev != NULL && (dev->get_device_type() != mcfg_DreamcastDevice(bus, port)
						|| maple_vmu_file_changed(dev)))
					mcfg_DestroyDevice(bus, port);
			}
		}
		if (!plug_in || mcfg_DreamcastDevice(bus, 5) == MDT_None)
			continue;
		if (MapleDevices[bus][5] == NULL)
			mcfg_Create(mcfg_DreamcastDevice(bus, 5), bus, 5);
		for (int port = 0; port < 5; port++)
		{
			const MapleDeviceType type = mcfg_DreamcastDevice(bus, port);
			if (type != MDT_None && MapleDevices[bus][port] == NULL)
				mcfg_Create(type, bus, port);
		}
	}
}

void mcfg_SerializeDevices(void **data, unsigned int *total_size)
{
	for (int i = 0; i < MAPLE_PORTS; i++)
		for (int j = 0; j < 6; j++)
		{
			u8 **p = (u8 **)data;
			if (MapleDevices[i][j] != NULL)
			{
				if (*p != NULL)
				{
					**p = MapleDevices[i][j]->get_device_type();
					*p = *p + 1;
				}
				MapleDevices[i][j]->maple_serialize(data, total_size);
			}
			else if (*p != NULL)
			{
				**p = MDT_None;
				*p = *p + 1;
			}
			*total_size = *total_size + 1;
		}
}

/* Whose inputs an Atomiswave's device on the third or fourth bus reads: the
 * first and second player's, where mcfg_CreateDevices() puts their analog
 * axes, guns or trackball there. A state does not say, and a device made
 * from one read the third and fourth player's. */
static int mcfg_PlayerNum(MapleDeviceType type, int bus)
{
	switch (settings.mapping.JammaSetup)
	{
	case JVS::Analog:
		return type == MDT_SegaController ? bus - 2 : -1;
	case JVS::LightGun:
		return type == MDT_LightGun ? bus - 2 : -1;
	case JVS::RotaryEncoders:
	case JVS::SegaMarineFishing:
		return type == MDT_Mouse && bus == 2 ? 0 : -1;
	default:
		return -1;
	}
}

void mcfg_UnserializeDevices(void **data, unsigned int *total_size)
{
	/* A NAOMI's I/O board, when it goes, forgets that the EEPROM has been
	 * read from its file. The board made below is the same one, and the
	 * EEPROM is what the state says: it has been read from the state by
	 * now, and with it whether it had been read from the file. */
	const bool eeprom_loaded = EEPROM_loaded;
	const bool atomiswave = settings.System == DC_PLATFORM_ATOMISWAVE;

	/* The devices are made anew, as what the state says is there. Not the
	 * memory cards: one that the state has too stays, and is given the
	 * state's contents. Making a card is reading its file, after waiting
	 * for everything still on its way there to be written - four or eight
	 * times for every state loaded, which with run-ahead is every frame -
	 * and all of what it then is comes from the state. */
	for (int i = 0; i < MAPLE_PORTS; i++)
		for (int j = 0; j < 6; j++)
			if (MapleDevices[i][j] != NULL && MapleDevices[i][j]->get_device_type() != MDT_SegaVMU)
				mcfg_DestroyDevice(i, j);
	EEPROM_loaded = eeprom_loaded;

	for (int i = 0; i < MAPLE_PORTS; i++)
		for (int j = 0; j < 6; j++)
		{
			/* Which device is there is a byte of the state's: past the end
			 * of a state that stops short there is none, and one that is no
			 * device at all is none either. (It was made all the same, which
			 * maple_Create() cannot do.) */
			u8 type = MDT_None;
			if (!LIBRETRO_US(type))
				type = MDT_None;
			if (type > MDT_None)
			{
				WARN_LOG(MAPLE, "Invalid device type %d at %d.%d in the state", type, i, j);
				type = MDT_None;
				// (what such a device has in the state comes next, and is read as something else: the load has failed)
				ra_unserialize_fail();
			}
			MapleDeviceType device_type = (MapleDeviceType)type;
			// (a card where the state has none, or something else)
			if (MapleDevices[i][j] != NULL && device_type != MDT_SegaVMU)
				mcfg_DestroyDevice(i, j);
			if (device_type != MDT_None)
			{
				if (MapleDevices[i][j] == NULL)
					mcfg_Create(device_type, i, j,
							atomiswave && i >= 2 && j == 5 ? mcfg_PlayerNum(device_type, i) : -1);
				MapleDevices[i][j]->maple_unserialize(data, total_size);
			}
		}
}
