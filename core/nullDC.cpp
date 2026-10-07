// nullDC.cpp : Makes magic cookies
//

//initialse Emu
#include <retro_atomic.h>
#include "types.h"
#include "hw/mem/_vmem.h"
#include "hw/mem/vmem32.h"
#include "stdclass.h"

#include "types.h"
#include "hw/flashrom/flashrom.h"
#include "hw/maple/maple_cfg.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/arm7/arm7.h"

#include "hw/naomi/naomi_cart.h"

#include "reios/reios.h"
#include <libretro.h>

extern RomChip sys_rom;
extern SRamChip sys_nvmem_sram;
extern DCFlashChip sys_nvmem_flash;
unsigned FLASH_SIZE;
unsigned BBSRAM_SIZE;
unsigned BIOS_SIZE;
unsigned RAM_SIZE;
unsigned ARAM_SIZE;
unsigned VRAM_SIZE;
unsigned RAM_MASK;
unsigned ARAM_MASK;
unsigned VRAM_MASK;

settings_t settings;

extern char game_dir[1024];
extern char *game_data;
extern bool boot_to_bios;
extern retro_atomic_int_t reset_requested;

/*
	libndc

	//initialise (and parse the command line)
	ndc_init(argc,argv);

	...
	//run a dreamcast slice
	//either a frame, or up to 25 ms of emulation
	//returns 1 if the frame is ready (fb needs to be flipped -- i'm looking at you android)
	ndc_step();

	...
	//terminate (and free everything)
	ndc_term()
*/

int GetFile(char *szFileName, char *szParse=0,u32 flags=0) 
{
   /* No disc image when the content is an ELF: the drive stays empty */
   if (!boot_to_bios && game_data)
   {
      strcpy(szFileName, game_data);
      strcpy(settings.imgread.DefaultImage, szFileName);
   }

	return 1; 
}


/* On failure nothing stays initialised: each plugin brought up before the
 * failing one is terminated again, so the caller sees the same state it
 * started from. */
s32 plugins_Init()
{
   s32 rv;

   if ((rv = libPvr_Init()))
      return rv;

   if ((rv = libGDR_Init()))
      goto fail_pvr;

   if (settings.System != DC_PLATFORM_DREAMCAST)
   {
      if (!naomi_cart_SelectFile())
      {
         rv = -1;
         goto fail_gdr;
      }
   }

   if ((rv = libAICA_Init()))
      goto fail_gdr;

   if ((rv = libARM_Init()))
      goto fail_aica;

   return 0;

fail_aica:
   libAICA_Term();
fail_gdr:
   naomi_cart_Close();
   libGDR_Term();
fail_pvr:
   libPvr_Term();
   return rv;
}

void plugins_Term(void)
{
   //term all plugins
   //libExtDevice_Term();
   
	//arm7_Term ?
   libAICA_Term();

   libGDR_Term();
   libPvr_Term();
}

void plugins_Reset(bool hard)
{
	libPvr_Reset(hard);
	libGDR_Reset(hard);
	libAICA_Reset(hard);
   libARM_Reset(hard);

	//libExtDevice_Reset(Manual);
}

#include "rom_luts.h"

static void LoadSpecialSettingsCPU(void)
{
#if FEAT_SHREC != DYNAREC_NONE
	if(settings.dynarec.Enable)
	{
		Get_Sh4Recompiler(&sh4_cpu);
		INFO_LOG(DYNAREC, "Using Recompiler");
	}
	else
#endif
	{
		Get_Sh4Interpreter(&sh4_cpu);
		INFO_LOG(INTERPRETER, "Using Interpreter");
	}
   sh4_cpu.Reset(false);
}

static void LoadSpecialSettings(void)
{
   unsigned i;

   char prod_id[sizeof(ip_meta.product_number) + 1] = {0};
   memcpy(prod_id, ip_meta.product_number, sizeof(ip_meta.product_number));

   NOTICE_LOG(BOOT, "[LUT]: Product number: %s.", prod_id);
	if (ip_meta.isWindowsCE() || settings.dreamcast.ForceWinCE
			|| !strncmp("T26702N", prod_id, 7)) // PBA Tour Bowling 2001
	{
		NOTICE_LOG(BOOT, "Enabling Full MMU and Extra depth scaling for Windows CE game");
		settings.rend.ExtraDepthScale = 0.1;
		settings.dreamcast.FullMMU = true;
	}
   for (i = 0; i < sizeof(lut_games)/sizeof(lut_games[0]); i++)
   {
      if (!strncmp(lut_games[i].product_number, prod_id, sizeof(prod_id) - 1))
      {
      	INFO_LOG(BOOT, "[LUT]: Found game in LUT database..");

         if (lut_games[i].alpha_sort_mode != -1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying alpha sort hack.");
            settings.pvr.Emulation.AlphaSortMode = lut_games[i].alpha_sort_mode;
         }


         if (lut_games[i].translucentPolygonDepthMask != -1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying translucent polygon depth mask hack.");
            settings.rend.TranslucentPolygonDepthMask = lut_games[i].translucentPolygonDepthMask;
         }

         if (lut_games[i].rendertotexturebuffer != -1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying rendertotexture hack.");
            settings.rend.RenderToTextureBuffer = lut_games[i].rendertotexturebuffer;
         }

         if (lut_games[i].disable_div != -1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying Disable DIV hack.");
         	settings.dynarec.ForceDisableDivMatching = settings.dynarec.DisableDivMatching = lut_games[i].disable_div;
         }
         if (lut_games[i].extra_depth_scale != 1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying auto extra depth scale.");
         	settings.rend.ExtraDepthScale = lut_games[i].extra_depth_scale;
         }
         if (lut_games[i].disable_vmem32 == 1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Disabling WinCE virtual memory.");
            settings.dynarec.disable_vmem32 = true;
         }
         if (lut_games[i].no_rgb_component == 1)
         {
         	NOTICE_LOG(BOOT, "Disabling RGB component output");
            settings.dreamcast.cable = 3;		// TV composite
         }

         break;
      }
   }
	std::string areas(ip_meta.area_symbols, sizeof(ip_meta.area_symbols));
	bool region_usa = areas.find('U') != std::string::npos;
	bool region_eu = areas.find('E') != std::string::npos;
	bool region_japan = areas.find('J') != std::string::npos;
	if (region_usa || region_eu || region_japan)
	{
		switch (settings.dreamcast.region)
		{
		case 0: // Japan
			if (!region_japan)
			{
				NOTICE_LOG(BOOT, "Japan region not supported. Using %s instead", region_usa ? "USA" : "Europe");
				settings.dreamcast.region = region_usa ? 1 : 2;
			}
			break;
		case 1: // USA
			if (!region_usa)
			{
				NOTICE_LOG(BOOT, "USA region not supported. Using %s instead", region_eu ? "Europe" : "Japan");
				settings.dreamcast.region = region_eu ? 2 : 0;
			}
			break;
		case 2: // Europe
			if (!region_eu)
			{
				NOTICE_LOG(BOOT, "Europe region not supported. Using %s instead", region_usa ? "USA" : "Japan");
				settings.dreamcast.region = region_usa ? 1 : 0;
			}
			break;
		case 3: // Default
			if (region_usa)
				settings.dreamcast.region = 1;
			else if (region_eu)
				settings.dreamcast.region = 2;
			else
				settings.dreamcast.region = 0;
				break;
		}
	}
	else
		WARN_LOG(BOOT, "No region specified in IP.BIN");
	if (settings.dreamcast.cable <= 1 && !ip_meta.supportsVGA())
	{
		NOTICE_LOG(BOOT, "Game doesn't support VGA. Using TV Composite instead");
		settings.dreamcast.cable = 3;
	}
}

static void LoadSpecialSettingsNaomi(const char *name)
{
   unsigned i;

   NOTICE_LOG(BOOT, "[LUT]: Naomi ROM name is: %s.", name);
   for (i = 0; i < sizeof(lut_games_naomi)/sizeof(lut_games_naomi[0]); i++)
   {
      if (strstr(lut_games_naomi[i].product_number, name))
      {
      	INFO_LOG(BOOT, "[LUT]: Found game in LUT database..");

         if (lut_games_naomi[i].alpha_sort_mode != -1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying alpha sort hack.");
            settings.pvr.Emulation.AlphaSortMode = lut_games_naomi[i].alpha_sort_mode;
         }


         if (lut_games_naomi[i].translucentPolygonDepthMask != -1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying translucent polygon depth mask hack.");
            settings.rend.TranslucentPolygonDepthMask = lut_games_naomi[i].translucentPolygonDepthMask;
         }

         if (lut_games_naomi[i].rendertotexturebuffer != -1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying rendertotexture hack.");
            settings.rend.RenderToTextureBuffer = lut_games_naomi[i].rendertotexturebuffer;
         }

         if (lut_games_naomi[i].disable_div != -1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying Disable DIV hack.");
         	settings.dynarec.ForceDisableDivMatching = settings.dynarec.DisableDivMatching = lut_games_naomi[i].disable_div;
         }

         if (lut_games_naomi[i].jamma_setup != JVS::Default)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying alternate Jamma I/O board setup.");
            settings.mapping.JammaSetup = lut_games_naomi[i].jamma_setup;
         }

         if (lut_games_naomi[i].extra_depth_scale != 1)
         {
         	NOTICE_LOG(BOOT, "[Hack]: Applying auto extra depth scale.");
            settings.rend.ExtraDepthScale = lut_games_naomi[i].extra_depth_scale;
         }

         if (lut_games_naomi[i].game_inputs != NULL)
         {
         	NOTICE_LOG(BOOT, "Setting custom input descriptors\n");
         	naomi_game_inputs = lut_games_naomi[i].game_inputs;
         }

         break;
      }
   }
}

void dc_prepare_system()
{
   BBSRAM_SIZE             = (32*1024);

   switch (settings.System)
   {
      case DC_PLATFORM_DREAMCAST:
         //DC : 16 mb ram, 8 mb vram, 2 mb aram, 2 mb bios, 128k flash
         FLASH_SIZE        = (128*1024);
         BIOS_SIZE         = (2*1024*1024);
         RAM_SIZE          = (16*1024*1024);
         ARAM_SIZE         = (2*1024*1024);
         VRAM_SIZE         = (8*1024*1024);
         sys_nvmem_flash.Allocate(FLASH_SIZE);
         sys_rom.Allocate(BIOS_SIZE);
         break;
      case DC_PLATFORM_DEV_UNIT:
         //Devkit : 32 mb ram, 8? mb vram, 2? mb aram, 2? mb bios, ? flash
         FLASH_SIZE        = (128*1024);
         BIOS_SIZE         = (2*1024*1024);
         RAM_SIZE          = (32*1024*1024);
         ARAM_SIZE         = (2*1024*1024);
         VRAM_SIZE         = (8*1024*1024);
         sys_nvmem_flash.Allocate(FLASH_SIZE);
         sys_rom.Allocate(BIOS_SIZE);
         break;
      case DC_PLATFORM_NAOMI:
         //Naomi : 32 mb ram, 16 mb vram, 8 mb aram, 2 mb bios, ? flash
         BIOS_SIZE         = (2*1024*1024);
         RAM_SIZE          = (32*1024*1024);
         ARAM_SIZE         = (8*1024*1024);
         VRAM_SIZE         = (16*1024*1024);
         sys_nvmem_sram.Allocate(BBSRAM_SIZE);
         sys_rom.Allocate(BIOS_SIZE);
         break;
      case DC_PLATFORM_NAOMI2:
         //Naomi2 : 32 mb ram, 16 mb vram, 8 mb aram, 2 mb bios, ? flash
         BIOS_SIZE         = (2*1024*1024);
         RAM_SIZE          = (32*1024*1024);
         ARAM_SIZE         = (8*1024*1024);
         VRAM_SIZE         = (16*1024*1024);
         sys_nvmem_sram.Allocate(BBSRAM_SIZE);
         sys_rom.Allocate(BIOS_SIZE);
         break;
      case DC_PLATFORM_ATOMISWAVE:
         //Atomiswave : 16 mb ram, 8 mb vram, 8 mb aram, 128kb bios+flash, 128k BBSRAM
         FLASH_SIZE        = 0;
         BIOS_SIZE         = (128*1024);
         RAM_SIZE          = (16*1024*1024);
         ARAM_SIZE         = (8*1024*1024);
         VRAM_SIZE         = (8*1024*1024);
         BBSRAM_SIZE       = (128*1024);
         sys_nvmem_flash.Allocate(BIOS_SIZE);
         sys_nvmem_flash.write_protect_size = BIOS_SIZE / 2;
         sys_nvmem_sram.Allocate(BBSRAM_SIZE);
         break;
   }

   RAM_MASK         = (RAM_SIZE-1);
   ARAM_MASK        = (ARAM_SIZE-1);
   VRAM_MASK        = (VRAM_SIZE-1);
}

void dc_reset(bool hard)
{
	plugins_Reset(hard);
	mem_Reset(hard);

	sh4_cpu.Reset(hard);
}

/* Stages of dc_init() that must be undone when a later one fails. */
enum
{
   DC_INIT_SETUP,    /* exception handlers installed */
   DC_INIT_VMEM,     /* + address space reserved */
   DC_INIT_CPU,      /* + sh4, memory map, unwind table */
   DC_INIT_PLUGINS   /* + plugins, cartridge */
};

static void dc_init_unwind(int stage)
{
   extern void common_libretro_cleanup(void);

   switch (stage)
   {
      case DC_INIT_PLUGINS:
         naomi_cart_Close();
         plugins_Term();
         /* fall through */
      case DC_INIT_CPU:
         sh4_cpu.Term();
         mem_Term();
         /* fall through */
      case DC_INIT_VMEM:
         _vmem_release();
         /* fall through */
      case DC_INIT_SETUP:
         common_libretro_cleanup();
         break;
   }
}

int dc_init()
{
   extern void common_libretro_setup(void);
   extern char game_dir_no_slash[1024];
   char new_system_dir[1024];

   setbuf(stdin,0);
   setbuf(stdout,0);
   setbuf(stderr,0);
   common_libretro_setup();

   if (!_vmem_reserve())
   {
      ERROR_LOG(VMEM, "Failed to alloc mem");
      dc_init_unwind(DC_INIT_SETUP);
      return -1;
   }
   reios_init();

   LoadSettings();

#ifdef _WIN32
   sprintf(new_system_dir, "%s\\", game_dir_no_slash);
#else
   sprintf(new_system_dir, "%s/", game_dir_no_slash);
#endif

   if (settings.System == DC_PLATFORM_DREAMCAST)
   {
      if (settings.bios.UseReios || !LoadRomFiles(new_system_dir))
      {
         /* Booting the BIOS requires a BIOS file */
         if (boot_to_bios || !LoadHle(new_system_dir))
         {
            dc_init_unwind(DC_INIT_VMEM);
            return -3;
         }
         WARN_LOG(COMMON, "Did not load bios, using reios");
      }
   }
   else
   {
      LoadRomFiles(new_system_dir);
   }
   LoadSpecialSettingsCPU();

   sh4_cpu.Init();
   mem_Init();

#ifdef _WIN64
   {
      extern void setup_seh();
      setup_seh();
   }
#endif

   if (plugins_Init())
   {
      dc_init_unwind(DC_INIT_CPU);
      return -4;
   }

   mem_map_default();

   dc_reset(true);

   switch (settings.System)
   {
      case DC_PLATFORM_DREAMCAST:
         if (libGDR_GetDiscType() == NoDisk && settings.reios.ElfFile.empty())
         {
            /* Content loading failed so force HLE off and boot the BIOS */
            settings.bios.UseReios = false;
            if (!LoadRomFiles(new_system_dir))
            {
               dc_init_unwind(DC_INIT_PLUGINS);
               return -3;
            }
            settings.imgread.DefaultImage[0] = '\0';
         }
         reios_disk_id();
         LoadSpecialSettings();
#if FEAT_SHREC != DYNAREC_NONE && HOST_CPU == CPU_ARM
         /* The 32-bit ARM recompiler knows nothing of the MMU: it goes on
          * reading and writing at the addresses a program gives as if
          * they were physical ones, and a Windows CE game, which lives
          * behind the MMU, cannot run. The interpreter does it properly.
          * Slow, on such a machine, but it is the game. */
         if (settings.dreamcast.FullMMU && settings.dynarec.Enable)
         {
            NOTICE_LOG(BOOT, "The 32-bit ARM recompiler has no MMU: using the interpreter for this game");
            sh4_cpu.Term();
            Get_Sh4Interpreter(&sh4_cpu);
            sh4_cpu.Init();
            sh4_cpu.Reset(false);
         }
#endif
         break;
      case DC_PLATFORM_ATOMISWAVE:
      case DC_PLATFORM_NAOMI:
         LoadSpecialSettingsNaomi(naomi_game_id);
         break;
   }
   FixUpFlash();

   mcfg_CreateDevices();

   return 0;
}

void dc_stop();

/*
 * Hard backstop guaranteeing dc_run() returns within a bounded emulated-cycle
 * budget so retro_run() can never be starved of control.
 *
 * Control normally returns at vblank (os_DoEvents), on whichever thread runs
 * the machine. That
 * relies on the SPG raising vblank, which needs valid video timing; before the
 * BIOS programs it, or if a title misprograms vstart, vblank may not fire and
 * dc_run() would otherwise run unbounded. This scheduler event forces a stop
 * after at most FRAME_DEADLINE_CYCLES and is re-armed at every dc_run() entry,
 * so under normal operation vblank always fires first (well inside the budget)
 * and it never triggers.
 */
#define FRAME_DEADLINE_CYCLES (SH4_MAIN_CLOCK / 20)	/* >= ~2.5 frames; catches a stalled vblank */

static int frame_deadline_schid = -1;

static int frame_deadline_sched(int tag, int c, int j)
{
	dc_stop();
	return 0;	/* one-shot; re-armed by dc_run() */
}

void dc_run()
{
	if (frame_deadline_schid == -1)
		frame_deadline_schid = sh4_sched_register(0, &frame_deadline_sched);
	sh4_sched_request(frame_deadline_schid, FRAME_DEADLINE_CYCLES);

	sh4_cpu.Run();
}

void dc_term()
{
	/* Remove the exception handler and dynarec unwind table before freeing the
	 * code cache and vmem they reference, so a stale handler can't fire during
	 * or after teardown. */
	extern void common_libretro_cleanup(void);
	common_libretro_cleanup();

	SaveRomFiles(get_writable_data_path(""));
	sh4_cpu.Term();
	naomi_cart_Close();
	plugins_Term();
	mem_Term();
	_vmem_release();
}

void dc_stop()
{
	sh4_cpu.Stop();
}


bool dc_is_running()
{
	return sh4_cpu.IsCpuRunning();
}

// Called on the emulator thread for soft reset
void dc_request_reset()
{
	retro_atomic_store_release_int(&reset_requested, 1);
	dc_stop();
}

void LoadSettings(void)
{
	settings.dynarec.Enable			= 1;
	//settings.dynarec.DisableDivMatching       = 0;
	//disable_nvmem can't be loaded, because nvmem init is before cfg load
	settings.dynarec.disable_vmem32 = false;
	settings.dreamcast.FullMMU		= false;
	//settings.pvr.Emulation.AlphaSortMode= 0;
	settings.rend.ExtraDepthScale        = 1.f;


	settings.rend.TranslucentPolygonDepthMask = false;

	settings.network.ActAsServer = false;
	settings.network.dns = "46.101.91.123";		// Dreamcast Live DNS
	settings.network.server = "";
	settings.network.EmulateBBA = false;
}
