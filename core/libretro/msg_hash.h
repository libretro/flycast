#ifndef __MSG_HASH_H
#define __MSG_HASH_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

#define MSG_HASH(Id, str) case Id: return str;

enum msg_hash_enums
{
   MSG_UNKNOWN = 0,

#if ((FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_ARM64) || (HOST_CPU == CPU_X64)) && defined(TARGET_NO_JIT)
   MSG_HASH_REICAST_CPU_MODE_DESC,
   MSG_HASH_OPTION_VAL_DYNAMIC_RECOMPILER,
   MSG_HASH_OPTION_VAL_GENERIC_RECOMPILER,
#endif
   MSG_HASH_REICAST_BOOT_TO_BIOS_DESC,
   MSG_HASH_REICAST_BOOT_TO_BIOS_INFO,
   MSG_HASH_REICAST_SYSTEM_DESC,
   MSG_HASH_OPTION_VAL_AUTO,
   MSG_HASH_OPTION_VAL_DREAMCAST,
   MSG_HASH_OPTION_VAL_NAOMI,
   MSG_HASH_OPTION_VAL_ATOMISWAVE,
   MSG_HASH_REICAST_HLE_BIOS_DESC,
   MSG_HASH_REICAST_HLE_BIOS_INFO,
#if defined(HAVE_OIT) || defined(HAVE_VULKAN)
   MSG_HASH_REICAST_OIT_ABUFFER_SIZE_DESC,
   MSG_HASH_OPTION_VAL_512MB,
   MSG_HASH_OPTION_VAL_1GB,
   MSG_HASH_OPTION_VAL_2GB,
   MSG_HASH_OPTION_VAL_4GB,
#endif
   MSG_HASH_REICAST_INTERNAL_RESOLUTION_DESC,
   MSG_HASH_REICAST_INTERNAL_RESOLUTION_INFO,
   MSG_HASH_OPTION_VAL_320X240,
   MSG_HASH_OPTION_VAL_640X480,
   MSG_HASH_OPTION_VAL_800X600,
   MSG_HASH_OPTION_VAL_960X720,
   MSG_HASH_OPTION_VAL_1024X768,
   MSG_HASH_OPTION_VAL_1280X960,
   MSG_HASH_OPTION_VAL_1440X1080,
   MSG_HASH_OPTION_VAL_1600X1200,
   MSG_HASH_OPTION_VAL_1920X1440,
   MSG_HASH_OPTION_VAL_2560X1920,
   MSG_HASH_OPTION_VAL_2880X2160,
   MSG_HASH_OPTION_VAL_3200X2400,
   MSG_HASH_OPTION_VAL_3840X2880,
   MSG_HASH_OPTION_VAL_4480X3360,
   MSG_HASH_OPTION_VAL_5120X3840,
   MSG_HASH_OPTION_VAL_5760X4320,
   MSG_HASH_OPTION_VAL_6400X4800,
   MSG_HASH_OPTION_VAL_7040X5280,
   MSG_HASH_OPTION_VAL_7680X5760,
   MSG_HASH_OPTION_VAL_8320X6240,
   MSG_HASH_OPTION_VAL_8960X6720,
   MSG_HASH_OPTION_VAL_9600X7200,
   MSG_HASH_OPTION_VAL_10240X7680,
   MSG_HASH_OPTION_VAL_10880X8160,
   MSG_HASH_OPTION_VAL_11520X8640,
   MSG_HASH_OPTION_VAL_12160X9120,
   MSG_HASH_OPTION_VAL_12800X9600,
   MSG_HASH_REICAST_SCREEN_ROTATION_DESC,
   MSG_HASH_OPTION_VAL_HORIZONTAL,
   MSG_HASH_OPTION_VAL_VERTICAL,
   MSG_HASH_REICAST_ALPHA_SORTING_DESC,
   MSG_HASH_OPTION_VAL_PERSTRIP_FAST_LEAST_ACCURATE,
   MSG_HASH_OPTION_VAL_PERTRIANGLE_NORMAL,
#if defined(HAVE_OIT) || defined(HAVE_VULKAN)
   MSG_HASH_OPTION_VAL_PERPIXEL_ACCURATE_BUT_SLOWEST,
#endif
   MSG_HASH_REICAST_GDROM_FAST_LOADING_DESC,
   MSG_HASH_REICAST_GDROM_FAST_LOADING_INFO,
   MSG_HASH_REICAST_MIPMAPPING_DESC,
   MSG_HASH_REICAST_FOG_DESC,
   MSG_HASH_REICAST_VOLUME_MODIFIER_ENABLE_DESC,
   MSG_HASH_REICAST_VOLUME_MODIFIER_ENABLE_INFO,
   MSG_HASH_REICAST_WIDESCREEN_HACK_DESC,
   MSG_HASH_REICAST_WIDESCREEN_CHEATS_DESC,
   MSG_HASH_REICAST_WIDESCREEN_CHEATS_INFO,
   MSG_HASH_REICAST_CABLE_TYPE_DESC,
   MSG_HASH_OPTION_VAL_TV_RGB,
   MSG_HASH_OPTION_VAL_TV_COMPOSITE,
   MSG_HASH_OPTION_VAL_VGA_RGB,
   MSG_HASH_REICAST_BROADCAST_DESC,
   MSG_HASH_OPTION_VAL_DEFAULT,
   MSG_HASH_OPTION_VAL_PALM_BRAZIL,
   MSG_HASH_OPTION_VAL_PALN_ARGENTINA_PARAGUAY_URUGUAY,
   MSG_HASH_OPTION_VAL_NTSC,
   MSG_HASH_OPTION_VAL_PAL_WORLD,
   MSG_HASH_REICAST_FRAMERATE_DESC,
   MSG_HASH_REICAST_FRAMERATE_INFO,
   MSG_HASH_OPTION_VAL_FULL_SPEED,
   MSG_HASH_OPTION_VAL_NORMAL,
   MSG_HASH_REICAST_REGION_DESC,
   MSG_HASH_OPTION_VAL_JAPAN,
   MSG_HASH_OPTION_VAL_USA,
   MSG_HASH_OPTION_VAL_EUROPE,
   MSG_HASH_REICAST_LANGUAGE_DESC,
   MSG_HASH_OPTION_VAL_JAPANESE,
   MSG_HASH_OPTION_VAL_ENGLISH,
   MSG_HASH_OPTION_VAL_GERMAN,
   MSG_HASH_OPTION_VAL_FRENCH,
   MSG_HASH_OPTION_VAL_SPANISH,
   MSG_HASH_OPTION_VAL_ITALIAN,
   MSG_HASH_REICAST_DIV_MATCHING_DESC,
   MSG_HASH_REICAST_DIV_MATCHING_INFO,
   MSG_HASH_REICAST_FORCE_WINCE_DESC,
   MSG_HASH_REICAST_FORCE_WINCE_INFO,
   MSG_HASH_REICAST_ANALOG_STICK_DEADZONE_DESC,
   MSG_HASH_OPTION_VAL_0,
   MSG_HASH_OPTION_VAL_5,
   MSG_HASH_OPTION_VAL_10,
   MSG_HASH_OPTION_VAL_15,
   MSG_HASH_OPTION_VAL_20,
   MSG_HASH_OPTION_VAL_25,
   MSG_HASH_OPTION_VAL_30,
   MSG_HASH_OPTION_VAL_40,
   MSG_HASH_OPTION_VAL_50,
   MSG_HASH_OPTION_VAL_60,
   MSG_HASH_OPTION_VAL_70,
   MSG_HASH_OPTION_VAL_80,
   MSG_HASH_OPTION_VAL_90,
   MSG_HASH_OPTION_VAL_100,
   MSG_HASH_REICAST_TRIGGER_DEADZONE_DESC,
   MSG_HASH_REICAST_DIGITAL_TRIGGERS_DESC,
   MSG_HASH_REICAST_ENABLE_DSP_DESC,
   MSG_HASH_REICAST_ENABLE_DSP_INFO,
   MSG_HASH_REICAST_ANISOTROPIC_FILTERING_DESC,
   MSG_HASH_REICAST_ANISOTROPIC_FILTERING_INFO,
   MSG_HASH_REICAST_PVR2_FILTERING_DESC,
   MSG_HASH_REICAST_PVR2_FILTERING_INFO,
#ifdef HAVE_TEXUPSCALE
   MSG_HASH_REICAST_TEXUPSCALE_DESC,
   MSG_HASH_REICAST_TEXUPSCALE_INFO,
   MSG_HASH_REICAST_TEXUPSCALE_MAX_FILTERED_TEXTURE_SIZE_DESC,
#endif
   MSG_HASH_REICAST_ENABLE_RTTB_DESC,
   MSG_HASH_REICAST_RENDER_TO_TEXTURE_UPSCALING_DESC,
   MSG_HASH_OPTION_VAL_1X,
   MSG_HASH_OPTION_VAL_2X,
   MSG_HASH_OPTION_VAL_3X,
   MSG_HASH_OPTION_VAL_4X,
   MSG_HASH_OPTION_VAL_5X,
   MSG_HASH_OPTION_VAL_6X,
   MSG_HASH_OPTION_VAL_8X,
#if !defined(TARGET_NO_THREADS)
   MSG_HASH_REICAST_THREADED_RENDERING_DESC,
   MSG_HASH_REICAST_THREADED_RENDERING_INFO,
   MSG_HASH_REICAST_SYNCHRONOUS_RENDERING_DESC,
   MSG_HASH_REICAST_SYNCHRONOUS_RENDERING_INFO,
   MSG_HASH_REICAST_DELAY_FRAME_SWAPPING_DESC,
   MSG_HASH_REICAST_DELAY_FRAME_SWAPPING_INFO,
#endif
   MSG_HASH_REICAST_FRAME_SKIPPING_DESC,
   MSG_HASH_REICAST_FRAME_SKIPPING_INFO,
   MSG_HASH_REICAST_ENABLE_PURUPURU_DESC,
   MSG_HASH_REICAST_ENABLE_PURUPURU_INFO,
   MSG_HASH_REICAST_ALLOW_SERVICE_BUTTONS_DESC,
   MSG_HASH_REICAST_ALLOW_SERVICE_BUTTONS_INFO,
   MSG_HASH_REICAST_ENABLE_NAOMI_15KHZ_DIPSWITCH_DESC,
   MSG_HASH_REICAST_ENABLE_NAOMI_15KHZ_DIPSWITCH_INFO,
   MSG_HASH_REICAST_CUSTOM_TEXTURES_DESC,
   MSG_HASH_REICAST_DUMP_TEXTURES_DESC,
   MSG_HASH_REICAST_PER_CONTENT_VMUS_DESC,
   MSG_HASH_REICAST_PER_CONTENT_VMUS_INFO,
   MSG_HASH_OPTION_VAL_VMU_A1,
   MSG_HASH_OPTION_VAL_ALL_VMUS,
   MSG_HASH_REICAST_SHOW_VMU_SCREEN_SETTINGS_DESC,
   MSG_HASH_REICAST_SHOW_VMU_SCREEN_SETTINGS_INFO,
   MSG_HASH_REICAST_VMU1_SCREEN_DISPLAY_DESC,
   MSG_HASH_REICAST_VMU1_SCREEN_POSITION_DESC,
   MSG_HASH_OPTION_VAL_UPPER_LEFT,
   MSG_HASH_OPTION_VAL_UPPER_RIGHT,
   MSG_HASH_OPTION_VAL_LOWER_LEFT,
   MSG_HASH_OPTION_VAL_LOWER_RIGHT,
   MSG_HASH_REICAST_VMU1_SCREEN_SIZE_MULT_DESC,
   MSG_HASH_REICAST_VMU1_PIXEL_ON_COLOR_DESC,
   MSG_HASH_OPTION_VAL_DEFAULT_ON,
   MSG_HASH_OPTION_VAL_DEFAULT_OFF,
   MSG_HASH_OPTION_VAL_BLACK,
   MSG_HASH_OPTION_VAL_BLUE,
   MSG_HASH_OPTION_VAL_LIGHT_BLUE,
   MSG_HASH_OPTION_VAL_GREEN,
   MSG_HASH_OPTION_VAL_CYAN,
   MSG_HASH_OPTION_VAL_CYAN_BLUE,
   MSG_HASH_OPTION_VAL_LIGHT_GREEN,
   MSG_HASH_OPTION_VAL_CYAN_GREEN,
   MSG_HASH_OPTION_VAL_LIGHT_CYAN,
   MSG_HASH_OPTION_VAL_RED,
   MSG_HASH_OPTION_VAL_PURPLE,
   MSG_HASH_OPTION_VAL_LIGHT_PURPLE,
   MSG_HASH_OPTION_VAL_YELLOW,
   MSG_HASH_OPTION_VAL_GRAY,
   MSG_HASH_OPTION_VAL_LIGHT_PURPLE_2,
   MSG_HASH_OPTION_VAL_LIGHT_GREEN_2,
   MSG_HASH_OPTION_VAL_LIGHT_GREEN_3,
   MSG_HASH_OPTION_VAL_LIGHT_CYAN_2,
   MSG_HASH_OPTION_VAL_LIGHT_RED_2,
   MSG_HASH_OPTION_VAL_MAGENTA,
   MSG_HASH_OPTION_VAL_LIGHT_ORANGE,
   MSG_HASH_OPTION_VAL_ORANGE,
   MSG_HASH_OPTION_VAL_LIGHT_PURPLE_3,
   MSG_HASH_OPTION_VAL_LIGHT_YELLOW,
   MSG_HASH_OPTION_VAL_LIGHT_YELLOW_2,
   MSG_HASH_OPTION_VAL_WHITE,
   MSG_HASH_REICAST_VMU1_PIXEL_OFF_COLOR_DESC,
   MSG_HASH_REICAST_VMU1_SCREEN_OPACITY_DESC,
   MSG_HASH_REICAST_VMU2_SCREEN_DISPLAY_DESC,
   MSG_HASH_REICAST_VMU2_SCREEN_POSITION_DESC,
   MSG_HASH_REICAST_VMU2_SCREEN_SIZE_MULT_DESC,
   MSG_HASH_REICAST_VMU2_PIXEL_ON_COLOR_DESC,
   MSG_HASH_REICAST_VMU2_PIXEL_OFF_COLOR_DESC,
   MSG_HASH_REICAST_VMU2_SCREEN_OPACITY_DESC,
   MSG_HASH_REICAST_VMU3_SCREEN_DISPLAY_DESC,
   MSG_HASH_REICAST_VMU3_SCREEN_POSITION_DESC,
   MSG_HASH_REICAST_VMU3_SCREEN_SIZE_MULT_DESC,
   MSG_HASH_REICAST_VMU3_PIXEL_ON_COLOR_DESC,
   MSG_HASH_REICAST_VMU3_PIXEL_OFF_COLOR_DESC,
   MSG_HASH_REICAST_VMU3_SCREEN_OPACITY_DESC,
   MSG_HASH_REICAST_VMU4_SCREEN_DISPLAY_DESC,
   MSG_HASH_REICAST_VMU4_SCREEN_POSITION_DESC,
   MSG_HASH_REICAST_VMU4_SCREEN_SIZE_MULT_DESC,
   MSG_HASH_REICAST_VMU4_PIXEL_ON_COLOR_DESC,
   MSG_HASH_REICAST_VMU4_PIXEL_OFF_COLOR_DESC,
   MSG_HASH_REICAST_VMU4_SCREEN_OPACITY_DESC,
   MSG_HASH_REICAST_SHOW_LIGHTGUN_SETTINGS_DESC,
   MSG_HASH_REICAST_SHOW_LIGHTGUN_SETTINGS_INFO,
   MSG_HASH_REICAST_LIGHTGUN1_CROSSHAIR_DESC,
   MSG_HASH_REICAST_LIGHTGUN2_CROSSHAIR_DESC,
   MSG_HASH_REICAST_LIGHTGUN3_CROSSHAIR_DESC,
   MSG_HASH_REICAST_LIGHTGUN4_CROSSHAIR_DESC,
   MSG_LAST,

   /* Ensure sizeof(enum) == sizeof(int) */
   MSG_DUMMY          = INT_MAX
};

static INLINE bool string_is_equal(const char *a, const char *b)
{
   return (a && b) ? !strcmp(a, b) : false;
}

/* Callback strings */

const char *msg_hash_to_str_ar(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_ar.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_ast(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_ast.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_chs(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_chs.h"
      default:
         break;
   }

   return "null";
}
const char *msg_hash_to_str_cht(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_cht.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_de(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_de.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_el(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_el.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_eo(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_eo.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_es(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_es.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_fa(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_fa.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_fi(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_fi.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_fr(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_fr.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_he(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_he.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_it(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_it.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_jp(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_jp.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_ko(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_ko.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_nl(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_nl.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_pl(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_pl.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_pt_br(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_pt_br.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_pt_pt(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_pt_pt.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_ru(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_ru.h"
      default:
         break;
   }

   return "null";
}

const char *msg_hash_to_str_sk(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_sk.h"
      default:
         break;
   }
   return "null";
}

const char *msg_hash_to_str_tr(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_tr.h"
      default:
         break;
   }
   return "null";
}

const char *msg_hash_to_str_us(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_us.h"
      default:
         break;
   }
   return "null";
}

const char *msg_hash_to_str_vn(enum msg_hash_enums msg)
{
   switch (msg)
   {
#include "intl/msg_hash_flycast_vn.h"
      default:
          break;
   }
   return "null";
}

const char *msg_hash_to_str(enum msg_hash_enums msg, unsigned language)
{
   const char *ret = NULL;

#ifndef HAVE_NO_LANGEXTRA
   switch (language)
   {
      case RETRO_LANGUAGE_ARABIC:
         ret = msg_hash_to_str_ar(msg);
         break;
      case RETRO_LANGUAGE_ASTURIAN:
         ret = msg_hash_to_str_ast(msg);
         break;
      case RETRO_LANGUAGE_CHINESE_SIMPLIFIED:
         ret = msg_hash_to_str_chs(msg);
         break;
      case RETRO_LANGUAGE_CHINESE_TRADITIONAL:
         ret = msg_hash_to_str_cht(msg);
         break;
      case RETRO_LANGUAGE_DUTCH:
         ret = msg_hash_to_str_nl(msg);
         break;
      case RETRO_LANGUAGE_ESPERANTO:
         ret = msg_hash_to_str_eo(msg);
         break;
      case RETRO_LANGUAGE_FINNISH:
         ret = msg_hash_to_str_fi(msg);
         break;
      case RETRO_LANGUAGE_FRENCH:
         ret = msg_hash_to_str_fr(msg);
         break;
      case RETRO_LANGUAGE_GERMAN:
         ret = msg_hash_to_str_de(msg);
         break;
      case RETRO_LANGUAGE_GREEK:
         ret = msg_hash_to_str_el(msg);
         break;
      case RETRO_LANGUAGE_HEBREW:
         ret = msg_hash_to_str_he(msg);
         break;
      case RETRO_LANGUAGE_ITALIAN:
         ret = msg_hash_to_str_it(msg);
         break;
      case RETRO_LANGUAGE_JAPANESE:
         ret = msg_hash_to_str_jp(msg);
         break;
      case RETRO_LANGUAGE_KOREAN:
         ret = msg_hash_to_str_ko(msg);
         break;
      case RETRO_LANGUAGE_PERSIAN:
         ret = msg_hash_to_str_fa(msg);
         break;
      case RETRO_LANGUAGE_POLISH:
         ret = msg_hash_to_str_pl(msg);
         break;
      case RETRO_LANGUAGE_PORTUGUESE_BRAZIL:
         ret = msg_hash_to_str_pt_br(msg);
         break;
      case RETRO_LANGUAGE_PORTUGUESE_PORTUGAL:
         ret = msg_hash_to_str_pt_pt(msg);
         break;
      case RETRO_LANGUAGE_RUSSIAN:
         ret = msg_hash_to_str_ru(msg);
         break;
      case RETRO_LANGUAGE_SLOVAK:
         ret = msg_hash_to_str_sk(msg);
         break;
      case RETRO_LANGUAGE_SPANISH:
         ret = msg_hash_to_str_es(msg);
         break;
      case RETRO_LANGUAGE_TURKISH:
         ret = msg_hash_to_str_tr(msg);
         break;
      case RETRO_LANGUAGE_VIETNAMESE:
         ret = msg_hash_to_str_vn(msg);
         break;
      default:
         break;
   }
#endif

   if (ret && !string_is_equal(ret, "null"))
      return ret;

   return msg_hash_to_str_us(msg);
}

#endif
