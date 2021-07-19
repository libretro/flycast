#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_inline.h>

#include "libretro_core_option_defines.h"

#ifndef HAVE_NO_LANGEXTRA
#include "libretro_core_options_intl.h"
#endif

/*
 ********************************
 * VERSION: 1.3
 ********************************
 *
 * - 1.3: Move translations to libretro_core_options_intl.h
 *        - libretro_core_options_intl.h includes BOM and utf-8
 *          fix for MSVC 2010-2013
 *        - Added HAVE_NO_LANGEXTRA flag to disable translations
 *          on platforms/compilers without BOM support
 * - 1.2: Use core options v1 interface when
 *        RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION is >= 1
 *        (previously required RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION == 1)
 * - 1.1: Support generation of core options v0 retro_core_option_value
 *        arrays containing options with a single value
 * - 1.0: First commit
*/

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
*/

/* RETRO_LANGUAGE_ENGLISH */

/* Default language:
 * - All other languages must include the same keys and values
 * - Will be used as a fallback in the event that frontend language
 *   is not available
 * - Will be used as a fallback for any missing entries in
 *   frontend language definition */

#define COLORS_STRING \
      { "BLACK 02",          "Black" }, \
      { "BLUE 03",           "Blue" }, \
      { "LIGHT_BLUE 04",     "Light Blue" }, \
      { "GREEN 05",          "Green" }, \
      { "CYAN 06",           "Cyan" }, \
      { "CYAN_BLUE 07",      "Cyan Blue" }, \
      { "LIGHT_GREEN 08",    "Light Green" }, \
      { "CYAN_GREEN 09",     "Cyan Green" }, \
      { "LIGHT_CYAN 10",     "Light Cyan" }, \
      { "RED 11",            "Red" }, \
      { "PURPLE 12",         "Purple" }, \
      { "LIGHT_PURPLE 13",   "Light Purple" }, \
      { "YELLOW 14",         "Yellow" }, \
      { "GRAY 15",           "Gray" }, \
      { "LIGHT_PURPLE_2 16", "Light Purple (2)" }, \
      { "LIGHT_GREEN_2 17",  "Light Green (2)" }, \
      { "LIGHT_GREEN_3 18",  "Light Green (3)" }, \
      { "LIGHT_CYAN_2 19",   "Light Cyan (2)" }, \
      { "LIGHT_RED_2 20",    "Light Red (2)" }, \
      { "MAGENTA 21",        "Magenta" }, \
      { "LIGHT_ORANGE 22",   "Light Orange" }, \
      { "ORANGE 23",         "Orange" }, \
      { "LIGHT_PURPLE_3 24", "Light Purple (3)" }, \
      { "LIGHT_YELLOW 25",   "Light Yellow" }, \
      { "LIGHT_YELLOW_2 26", "Light Yellow (2)" }, \
      { "WHITE 27",          "White" }, \
      { NULL, NULL },

#define VMU_SCREEN_PARAMS(num) \
{ \
   CORE_OPTION_NAME "_vmu" #num "_screen_display", \
   "VMU Screen " #num " Display", \
   "", \
   { \
      { "disabled", NULL }, \
      { "enabled",  NULL }, \
      { NULL, NULL }, \
   }, \
   "disabled", \
}, \
{ \
   CORE_OPTION_NAME "_vmu" #num "_screen_position", \
   "VMU Screen " #num " Position", \
   "", \
   { \
      { "Upper Left",  NULL }, \
      { "Upper Right", NULL }, \
      { "Lower Left",  NULL }, \
      { "Lower Right", NULL }, \
      { NULL, NULL }, \
   }, \
   "Upper Left", \
}, \
{ \
   CORE_OPTION_NAME "_vmu" #num "_screen_size_mult", \
   "VMU Screen " #num " Size", \
   "", \
   { \
      { "1x", NULL }, \
      { "2x", NULL }, \
      { "3x", NULL }, \
      { "4x", NULL }, \
      { "5x", NULL }, \
      { NULL, NULL }, \
   }, \
   "1x", \
}, \
{ \
   CORE_OPTION_NAME "_vmu" #num "_pixel_on_color", \
   "VMU Screen " #num " Pixel On Color", \
   "", \
   { \
      { "DEFAULT_ON 00",  "Default ON" }, \
      { "DEFAULT_OFF 01", "Default OFF" }, \
      COLORS_STRING \
   }, \
   "DEFAULT_ON 00", \
}, \
{ \
   CORE_OPTION_NAME "_vmu" #num "_pixel_off_color", \
   "VMU Screen " #num " Pixel Off Color", \
   "", \
   { \
      { "DEFAULT_OFF 01", "Default OFF" }, \
      { "DEFAULT_ON 00",  "Default ON" }, \
      COLORS_STRING \
   }, \
   "DEFAULT_OFF 01", \
}, \
{ \
   CORE_OPTION_NAME "_vmu" #num "_screen_opacity", \
   "VMU Screen " #num " Opacity", \
   "", \
   { \
      { "10%",  NULL }, \
      { "20%",  NULL }, \
      { "30%",  NULL }, \
      { "40%",  NULL }, \
      { "50%",  NULL }, \
      { "60%",  NULL }, \
      { "70%",  NULL }, \
      { "80%",  NULL }, \
      { "90%",  NULL }, \
      { "100%", NULL }, \
      { NULL,   NULL }, \
   }, \
   "100%", \
},

#define LIGHTGUN_PARAMS(num) \
{ \
   CORE_OPTION_NAME "_lightgun" #num "_crosshair", \
   "Gun Crosshair " #num " Display", \
   "", \
   { \
      { "disabled", NULL }, \
      { "White",    NULL }, \
      { "Red",      NULL }, \
      { "Green",    NULL }, \
      { "Blue",     NULL }, \
      { NULL,       NULL }, \
   }, \
   "disabled", \
},

struct retro_core_option_definition option_defs[] = {
#if ((FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_ARM64) || (HOST_CPU == CPU_X64)) && defined(TARGET_NO_JIT)
   {
      CORE_OPTION_NAME "_cpu_mode",
      "CPU Mode (Restart)",
      "",
      {
#if (FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_ARM64) || (HOST_CPU == CPU_X64)
         { "dynamic_recompiler", "Dynamic Recompiler" },
#endif
#ifdef TARGET_NO_JIT
         { "generic_recompiler", "Generic Recompiler" },
#endif
         { NULL, NULL },
      },
#if (FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_ARM64) || (HOST_CPU == CPU_X64)
      "dynamic_recompiler",
#elif defined(TARGET_NO_JIT)
      "generic_recompiler",
#endif
   },
#endif
   {
      CORE_OPTION_NAME "_boot_to_bios",
      "Boot to BIOS (Restart)",
      "Boot directly into the Dreamcast BIOS menu.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_system",
      "System Type (Restart)",
      "",
      {
         { "auto",       "Auto" },
         { "dreamcast",  "Dreamcast" },
         { "naomi",      "NAOMI" },
         { "atomiswave", "Atomiswave" },
         { NULL, NULL },
      },
      "auto",
   },
   {
      CORE_OPTION_NAME "_hle_bios",
      "HLE BIOS",
      "Force use of high-level emulation BIOS.",
      {
         { "disabled",  NULL },
         { "enabled",  NULL },
         { NULL, NULL},
      },
      "disabled",
   },
#if defined(HAVE_OIT) || defined(HAVE_VULKAN)
   {
      CORE_OPTION_NAME "_oit_abuffer_size",
      "Accumulation Pixel Buffer Size (Restart)",
      "",
      {
         { "512MB", NULL },
         { "1GB",   NULL },
         { "2GB",   NULL },
         { "4GB",   NULL },
         { NULL, NULL },
      },
      "512MB",
   },
#endif
   {
      CORE_OPTION_NAME "_internal_resolution",
      "Internal Resolution (Restart)",
      "Modify rendering resolution. Requires a restart.",
      {
         { "320x240",    NULL },
         { "640x480",    NULL },
         { "800x600",    NULL },
         { "960x720",    NULL },
         { "1024x768",   NULL },
         { "1280x960",   NULL },
         { "1440x1080",  NULL },
         { "1600x1200",  NULL },
         { "1920x1440",  NULL },
         { "2560x1920",  NULL },
         { "2880x2160",  NULL },
         { "3200x2400",  NULL },
         { "3840x2880",  NULL },
         { "4480x3360",  NULL },
         { "5120x3840",  NULL },
         { "5760x4320",  NULL },
         { "6400x4800",  NULL },
         { "7040x5280",  NULL },
         { "7680x5760",  NULL },
         { "8320x6240",  NULL },
         { "8960x6720",  NULL },
         { "9600x7200",  NULL },
         { "10240x7680", NULL },
         { "10880x8160", NULL },
         { "11520x8640", NULL },
         { "12160x9120", NULL },
         { "12800x9600", NULL },
         { NULL, NULL },
      },
#ifdef LOW_RES
      "320x240",
#else
      "640x480",
#endif
   },
   {
      CORE_OPTION_NAME "_screen_rotation",
      "Screen Orientation",
      "",
      {
         { "horizontal", "Horizontal" },
         { "vertical",   "Vertical" },
         { NULL, NULL },
      },
      "horizontal",
   },
   {
      CORE_OPTION_NAME "_alpha_sorting",
      "Alpha Sorting",
      "",
      {
         { "per-strip (fast, least accurate)", "Per-Strip (fast, least accurate)" },
         { "per-triangle (normal)",            "Per-Triangle (normal)" },
#if defined(HAVE_OIT) || defined(HAVE_VULKAN)
         { "per-pixel (accurate)",             "Per-Pixel (accurate, but slowest)" },
#endif
         { NULL, NULL },
      },
#if defined(LOW_END)
      "per-strip (fast, least accurate)",
#else
      "per-triangle (normal)",
#endif
   },
   {
      CORE_OPTION_NAME "_gdrom_fast_loading",
      "GDROM Fast Loading (inaccurate)",
      "Speeds up GD-ROM loading.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
#ifdef LOW_END
      "enabled",
#else
      "disabled",
#endif
   },
   {
      CORE_OPTION_NAME "_mipmapping",
      "Mipmapping",
      "",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled",
   },
   {
      CORE_OPTION_NAME "_fog",
      "Fog Effects",
      "",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled",
   },
   {
      CORE_OPTION_NAME "_volume_modifier_enable",
      "Volume Modifier",
      "A Dreamcast GPU feature that is typically used by games to draw object shadows. This should normally be enabled - the performance impact is usually minimal to negligible.",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL },
      },
      "enabled",
   },
   {
      CORE_OPTION_NAME "_widescreen_hack",
      "Widescreen Hack (Restart)",
      "",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_widescreen_cheats",
      "Widescreen Cheats (Restart)",
      "Activates cheats that allow certain games to display in widescreen format.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_cable_type",
      "Cable Type",
      "",
      {
         { "TV (RGB)",       NULL },
         { "TV (Composite)", NULL },
         { "VGA (RGB)",      NULL },
         { NULL, NULL },
      },
#ifdef LOW_END
      "VGA (RGB)",
#else
      "TV (Composite)",
#endif
   },
   {
      CORE_OPTION_NAME "_broadcast",
      "Broadcast",
      "",
      {
         { "Default", NULL },
         { "PAL_M",   "PAL-M (Brazil)" },
         { "PAL_N",   "PAL-N (Argentina, Paraguay, Uruguay)" },
         { "NTSC",    NULL },
         { "PAL",     "PAL (World)" },
         { NULL, NULL },
      },
      "Default",
   },
   {
      CORE_OPTION_NAME "_framerate",
      "Framerate",
      "Affects how emulator interacts with frontend. 'Full Speed' - emulator returns control to RetroArch each time a frame has been rendered. 'Normal' - emulator returns control to RetroArch each time a V-blank interrupt is generated. 'Full Speed' should be used in most cases. 'Normal' may improve frame pacing on some systems, but can cause unresponsive input when screen is static (e.g. loading/pause screens). Note: This setting only applies when 'Threaded Rendering' is disabled.",
      {
         { "fullspeed", "Full Speed" },
         { "normal",    "Normal" },
         { NULL, NULL },
      },
      "fullspeed",
   },
   {
      CORE_OPTION_NAME "_region",
      "Region",
      "",
      {
         { "Default", NULL },
         { "Japan",   NULL },
         { "USA",     NULL },
         { "Europe",  NULL },
         { NULL, NULL },
      },
      "Default",
   },
   {
      CORE_OPTION_NAME "_language",
      "Language",
      "",
      {
         { "Default",  NULL },
         { "Japanese", NULL },
         { "English",  NULL },
         { "German",   NULL },
         { "French",   NULL },
         { "Spanish",  NULL },
         { "Italian",  NULL },
         { NULL, NULL },
      },
      "Default",
   },
   {
      CORE_OPTION_NAME "_div_matching",
      "DIV Matching",
      "Optimize integer division",
      {
         { "disabled", NULL },
         { "auto",     "Auto" },
         { NULL, NULL },
      },
      "auto",
   },
   {
      CORE_OPTION_NAME "_force_wince",
      "Force Windows CE Mode",
      "Enable full MMU emulation and other settings for Windows CE games",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_analog_stick_deadzone",
      "Analog Stick Deadzone",
      "",
      {
         { "0%",  NULL },
         { "5%",  NULL },
         { "10%", NULL },
         { "15%", NULL },
         { "20%", NULL },
         { "25%", NULL },
         { "30%", NULL },
         { NULL, NULL },
      },
      "15%",
   },
   {
      CORE_OPTION_NAME "_trigger_deadzone",
      "Trigger Deadzone",
      "",
      {
         { "0%",  NULL },
         { "5%",  NULL },
         { "10%", NULL },
         { "15%", NULL },
         { "20%", NULL },
         { "25%", NULL },
         { "30%", NULL },
         { NULL, NULL },
      },
      "0%",
   },
   {
      CORE_OPTION_NAME "_digital_triggers",
      "Digital Triggers",
      "",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_enable_dsp",
      "Enable DSP",
      "Enable emulation of the Dreamcast's audio DSP (digital signal processor). Improves the accuracy of generated sound, but increases performance requirements.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
#ifdef LOW_END
      "disabled",
#else
      "enabled",
#endif
   },
   {
      CORE_OPTION_NAME "_anisotropic_filtering",
      "Anisotropic Filtering",
      "Enhance the quality of textures on surfaces that are at oblique viewing angles with respect to the camera.",
      {
         { "off", "disabled" },
         { "2",  NULL },
         { "4",  NULL },
         { "8",  NULL },
         { "16",  NULL },
         { NULL, NULL },
      },
      "4",
   },
   {
      CORE_OPTION_NAME "_pvr2_filtering",
      "PowerVR2 Post-processing Filter",
      "Post-process the rendered image to simulate effects specific to the PowerVR2 GPU and analog video signals.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
#ifdef HAVE_TEXUPSCALE
   {
      CORE_OPTION_NAME "_texupscale",
      "Texture Upscaling (xBRZ)",
      "Enhance hand-drawn 2D pixel art graphics. Should only be used with 2D pixelized games.",
      {
         { "off", "disabled" },
         { "2x",  NULL },
         { "4x",  NULL },
         { "6x",  NULL },
         { NULL, NULL },
      },
      "off",
   },
   {
      CORE_OPTION_NAME "_texupscale_max_filtered_texture_size",
      "Texture Upscaling Max. Filtered Size",
      "",
      {
         { "256",  NULL },
         { "512",  NULL },
         { "1024", NULL },
         { NULL, NULL },
      },
      "256",
   },
#endif
   {
      CORE_OPTION_NAME "_enable_rttb",
      "Enable RTT (Render To Texture) Buffer",
      "",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_render_to_texture_upscaling",
      "Render To Texture Upscaling",
      "",
      {
         { "1x", NULL },
         { "2x", NULL },
         { "3x", NULL },
         { "4x", NULL },
         { "8x", NULL },
         { NULL, NULL },
      },
      "1x",
   },
#if !defined(TARGET_NO_THREADS)
   {
      CORE_OPTION_NAME "_threaded_rendering",
      "Threaded Rendering (Restart)",
      "Runs the GPU and CPU on different threads. Highly recommended.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled",
   },
   {
      CORE_OPTION_NAME "_synchronous_rendering",
      "Synchronous Rendering",
      "Waits for the GPU to finish rendering the previous frame instead of dropping the current one. Note: This setting only applies when 'Threaded Rendering' is enabled.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
#ifdef LOW_END
      "disabled",
#else
      "enabled",
#endif
   },
   {
      CORE_OPTION_NAME "_delay_frame_swapping",
      "Delay Frame Swapping",
      "Useful to avoid flashing screens or glitchy videos. Not recommended on slow platforms. Note: This setting only applies when 'Threaded Rendering' is enabled.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
#endif
   {
      CORE_OPTION_NAME "_frame_skipping",
      "Frame Skipping",
      "Sets the number of frames to skip between each displayed frame.",
      {
         { "disabled",  NULL },
         { "1",         NULL },
         { "2",         NULL },
         { "3",         NULL },
         { "4",         NULL },
         { "5",         NULL },
         { "6",         NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_enable_purupuru",
      "Purupuru Pack/Vibration Pack",
      "Enables controller force feedback.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "enabled",
   },
   {
      CORE_OPTION_NAME "_allow_service_buttons",
      "Allow NAOMI Service Buttons",
      "Enables SERVICE button for NAOMI, to enter cabinet settings.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_enable_naomi_15khz_dipswitch",
      "Enable NAOMI 15KHz Dipswitch",
      "This can force display in 240p, 480i or no effect at all depending on the game.",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_custom_textures",
      "Load Custom Textures",
      "",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_dump_textures",
      "Dump Textures",
      "",
      {
         { "disabled", NULL },
         { "enabled",  NULL },
         { NULL, NULL },
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_per_content_vmus",
      "Per-Game VMUs",
      "When disabled, all games share 4 VMU save files (A1, B1, C1, D1) located in RetroArch's system directory. The 'VMU A1' setting creates a unique VMU 'A1' file in RetroArch's save directory for each game that is launched. The 'All VMUs' setting creates 4 unique VMU files (A1, B1, C1, D1) for each game that is launched.",
      {
         { "disabled", NULL },
         { "VMU A1",   NULL },
         { "All VMUs", NULL },
         { NULL, NULL},
      },
      "disabled",
   },
   {
      CORE_OPTION_NAME "_show_vmu_screen_settings",
      "Show VMU Display Settings",
      "Enable configuration of emulated VMU LCD screen visibility, size, position and color. NOTE: Quick Menu must be toggled for this setting to take effect.",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL},
      },
      "disabled"
   },
   VMU_SCREEN_PARAMS(1)
   VMU_SCREEN_PARAMS(2)
   VMU_SCREEN_PARAMS(3)
   VMU_SCREEN_PARAMS(4)
   {
      CORE_OPTION_NAME "_show_lightgun_settings",
      "Show Light Gun Settings",
      "Enable configuration of light gun crosshair display options. NOTE: Quick Menu must be toggled for this setting to take effect.",
      {
         { "enabled",  NULL },
         { "disabled", NULL },
         { NULL, NULL},
      },
      "disabled"
   },
   LIGHTGUN_PARAMS(1)
   LIGHTGUN_PARAMS(2)
   LIGHTGUN_PARAMS(3)
   LIGHTGUN_PARAMS(4)
   { NULL, NULL, NULL, {{0}}, NULL },
};

/*
 ********************************
 * Functions
 ********************************
*/

/* Handles configuration/setting of core options.
 * Should be called as early as possible - ideally inside
 * retro_set_environment(), and no later than retro_load_game()
 * > We place the function body in the header to avoid the
 *   necessity of adding more .c files (i.e. want this to
 *   be as painless as possible for core devs)
 */

static INLINE void libretro_set_core_options(retro_environment_t environ_cb)
{
   unsigned version = 0;

   if (!environ_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && (version >= 1))
   {
      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, &option_defs);
   }
   else
   {
      size_t i;
      size_t option_index              = 0;
      size_t num_options               = 0;
      struct retro_variable *variables = NULL;
      char **values_buf                = NULL;

      /* Determine number of options
       * > Note: We are going to skip a number of irrelevant
       *   core options when building the retro_variable array,
       *   but we'll allocate space for all of them. The difference
       *   in resource usage is negligible, and this allows us to
       *   keep the code 'cleaner' */
      while (true)
      {
         if (option_defs[num_options].key)
            num_options++;
         else
            break;
      }

      /* Allocate arrays */
      variables  = (struct retro_variable *)calloc(num_options + 1, sizeof(struct retro_variable));
      values_buf = (char **)calloc(num_options, sizeof(char *));

      if (!variables || !values_buf)
         goto error;

      /* Copy parameters from option_defs array */
      for (i = 0; i < num_options; i++)
      {
         const char *key                        = option_defs[i].key;
         const char *desc                       = option_defs[i].desc;
         const char *default_value              = option_defs[i].default_value;
         struct retro_core_option_value *values = option_defs[i].values;
         size_t buf_len                         = 3;
         size_t default_index                   = 0;

         values_buf[i] = NULL;

         /* Skip options that are irrelevant when using the
          * old style core options interface */
         if ((strcmp(key, CORE_OPTION_NAME "_show_vmu_screen_settings") == 0) ||
             (strcmp(key, CORE_OPTION_NAME "_show_lightgun_settings") == 0))
            continue;

         if (desc)
         {
            size_t num_values = 0;

            /* Determine number of values */
            while (true)
            {
               if (values[num_values].value)
               {
                  /* Check if this is the default value */
                  if (default_value)
                     if (strcmp(values[num_values].value, default_value) == 0)
                        default_index = num_values;

                  buf_len += strlen(values[num_values].value);
                  num_values++;
               }
               else
                  break;
            }

            /* Build values string */
            if (num_values > 0)
            {
               size_t j;

               buf_len += num_values - 1;
               buf_len += strlen(desc);

               values_buf[i] = (char *)calloc(buf_len, sizeof(char));
               if (!values_buf[i])
                  goto error;

               strcpy(values_buf[i], desc);
               strcat(values_buf[i], "; ");

               /* Default value goes first */
               strcat(values_buf[i], values[default_index].value);

               /* Add remaining values */
               for (j = 0; j < num_values; j++)
               {
                  if (j != default_index)
                  {
                     strcat(values_buf[i], "|");
                     strcat(values_buf[i], values[j].value);
                  }
               }
            }
         }

         variables[option_index].key   = key;
         variables[option_index].value = values_buf[i];
         option_index++;
      }

      /* Set variables */
      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);

error:

      /* Clean up */
      if (values_buf)
      {
         for (i = 0; i < num_options; i++)
         {
            if (values_buf[i])
            {
               free(values_buf[i]);
               values_buf[i] = NULL;
            }
         }

         free(values_buf);
         values_buf = NULL;
      }

      if (variables)
      {
         free(variables);
         variables = NULL;
      }
   }
}

#ifdef __cplusplus
}
#endif

#endif
