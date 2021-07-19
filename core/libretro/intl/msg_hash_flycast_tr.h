#if defined(_MSC_VER) && !defined(_XBOX) && (_MSC_VER >= 1500 && _MSC_VER < 1900)
#if (_MSC_VER >= 1700)
/* https://support.microsoft.com/en-us/kb/980263 */
#pragma execution_character_set("utf-8")
#endif
#pragma warning(disable:4566)
#endif

#if ((FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_ARM64) || (HOST_CPU == CPU_X64)) && defined(TARGET_NO_JIT)
MSG_HASH(
   MSG_HASH_REICAST_CPU_MODE_DESC,
   "CPU Modu (Yeniden Başlatma Gerektirir)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_DYNAMIC_RECOMPILER,
   "Dinamik Yeniden Derleyici"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_GENERIC_RECOMPILER,
   "Jenerik Yeniden Derleyici"
   )
#endif
MSG_HASH(
   MSG_HASH_REICAST_BOOT_TO_BIOS_DESC,
   "BIOS'a önyükleme (Yeniden Başlatma Gerektirir)"
   )
MSG_HASH(
   MSG_HASH_REICAST_BOOT_TO_BIOS_INFO,
   "Doğrudan Dreamcast BIOS menüsüne önyükleme yapın."
   )
MSG_HASH(
   MSG_HASH_REICAST_SYSTEM_DESC,
   "Sistem Tipi (Yeniden Başlatma Gerektirir)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_AUTO,
   "Otomatik"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_DREAMCAST,
   "Dreamcast"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_NAOMI,
   "NAOMI"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_ATOMISWAVE,
   "Atomiswave"
   )
MSG_HASH(
   MSG_HASH_REICAST_HLE_BIOS_DESC,
   "HLE BIOS"
   )
MSG_HASH(
   MSG_HASH_REICAST_HLE_BIOS_INFO,
   "Üst düzey öykünmüş BIOS(HLE) kullanımını zorla."
   )
#if defined(HAVE_OIT) || defined(HAVE_VULKAN)
MSG_HASH(
   MSG_HASH_REICAST_OIT_ABUFFER_SIZE_DESC,
   "Birikim Piksel Arabellek Boyutu (Yeniden Başlatma Gerektirir)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_512MB,
   "512MB"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_1GB,
   "1GB"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_2GB,
   "2GB"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_4GB,
   "4GB"
   )
#endif
MSG_HASH(
   MSG_HASH_REICAST_INTERNAL_RESOLUTION_DESC,
   "Dahili Çözünürlük (Yeniden Başlat Gerektirir)"
   )
MSG_HASH(
   MSG_HASH_REICAST_INTERNAL_RESOLUTION_INFO,
   "Render çözünürlüğünü değiştirin. Yeniden başlatma gerektirir."
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_320X240,
   "320x240"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_640X480,
   "640x480"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_800X600,
   "800x600"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_960X720,
   "960x720"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_1024X768,
   "1024x768"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_1280X960,
   "1280x960"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_1440X1080,
   "1440x1080"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_1600X1200,
   "1600x1200"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_1920X1440,
   "1920x1440"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_2560X1920,
   "2560x1920"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_2880X2160,
   "2880x2160"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_3200X2400,
   "3200x2400"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_3840X2880,
   "3840x2880"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_4480X3360,
   "4480x3360"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_5120X3840,
   "5120x3840"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_5760X4320,
   "5760x4320"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_6400X4800,
   "6400x4800"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_7040X5280,
   "7040x5280"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_7680X5760,
   "7680x5760"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_8320X6240,
   "8320x6240"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_8960X6720,
   "8960x6720"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_9600X7200,
   "9600x7200"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_10240X7680,
   "10240x7680"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_10880X8160,
   "10880x8160"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_11520X8640,
   "11520x8640"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_12160X9120,
   "12160x9120"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_12800X9600,
   "12800x9600"
   )
MSG_HASH(
   MSG_HASH_REICAST_SCREEN_ROTATION_DESC,
   "Ekran Yönü"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_HORIZONTAL,
   "Yatay"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_VERTICAL,
   "Dikey"
   )
MSG_HASH(
   MSG_HASH_REICAST_ALPHA_SORTING_DESC,
   "Alfa Sıralama"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_PERSTRIP_FAST_LEAST_ACCURATE,
   "Şerit Başına (hızlı, en az doğru)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_PERTRIANGLE_NORMAL,
   "Üçgen Başına (normal)"
   )
#if defined(HAVE_OIT) || defined(HAVE_VULKAN)
MSG_HASH(
   MSG_HASH_OPTION_VAL_PERPIXEL_ACCURATE_BUT_SLOWEST,
   "Piksel Başına (doğru, ancak en yavaş)"
   )
#endif
MSG_HASH(
   MSG_HASH_REICAST_GDROM_FAST_LOADING_DESC,
   "GDROM Fast Loading (inaccurate)"
   )
MSG_HASH(
   MSG_HASH_REICAST_GDROM_FAST_LOADING_INFO,
   "Speeds up GD-ROM loading."
   )
MSG_HASH(
   MSG_HASH_REICAST_MIPMAPPING_DESC,
   "Mipmapping"
   )
MSG_HASH(
   MSG_HASH_REICAST_FOG_DESC,
   "Fog Effects"
   )
MSG_HASH(
   MSG_HASH_REICAST_VOLUME_MODIFIER_ENABLE_DESC,
   "Volume Modifier"
   )
MSG_HASH(
   MSG_HASH_REICAST_VOLUME_MODIFIER_ENABLE_INFO,
   "A Dreamcast GPU feature that is typically used by games to draw object shadows. This should normally be enabled - the performance impact is usually minimal to negligible."
   )
MSG_HASH(
   MSG_HASH_REICAST_WIDESCREEN_HACK_DESC,
   "Widescreen Hack (Restart Required)"
   )
MSG_HASH(
   MSG_HASH_REICAST_WIDESCREEN_CHEATS_DESC,
   "Widescreen Cheats (Restart Required)"
   )
MSG_HASH(
   MSG_HASH_REICAST_WIDESCREEN_CHEATS_INFO,
   "Activates cheats that allow certain games to display in widescreen format."
   )
MSG_HASH(
   MSG_HASH_REICAST_CABLE_TYPE_DESC,
   "Cable Type"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_TV_RGB,
   "TV (RGB)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_TV_COMPOSITE,
   "TV (Composite)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_VGA_RGB,
   "VGA (RGB)"
   )
MSG_HASH(
   MSG_HASH_REICAST_BROADCAST_DESC,
   "Broadcast"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_DEFAULT,
   "Default"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_PALM_BRAZIL,
   "PAL-M (Brazil)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_PALN_ARGENTINA_PARAGUAY_URUGUAY,
   "PAL-N (Argentina, Paraguay, Uruguay)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_NTSC,
   "NTSC"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_PAL_WORLD,
   "PAL (World)"
   )
MSG_HASH(
   MSG_HASH_REICAST_FRAMERATE_DESC,
   "Framerate"
   )
MSG_HASH(
   MSG_HASH_REICAST_FRAMERATE_INFO,
   "Affects how the emulator interacts with the frontend. 'Full Speed' - emulator returns control to RetroArch each time a frame has been rendered. 'Normal' - emulator returns control to RetroArch each time a V-blank interrupt is generated. 'Full Speed' should be used in most cases. 'Normal' may improve frame pacing on some systems, but can cause unresponsive input when screen is static (e.g. loading/pause screens). Note: This setting only applies when 'Threaded Rendering' is disabled."
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_FULL_SPEED,
   "Full Speed"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_NORMAL,
   "Normal"
   )
MSG_HASH(
   MSG_HASH_REICAST_REGION_DESC,
   "Region"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_JAPAN,
   "Japan"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_USA,
   "USA"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_EUROPE,
   "Europe"
   )
MSG_HASH(
   MSG_HASH_REICAST_LANGUAGE_DESC,
   "Language"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_JAPANESE,
   "Japanese"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_ENGLISH,
   "English"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_GERMAN,
   "German"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_FRENCH,
   "French"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_SPANISH,
   "Spanish"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_ITALIAN,
   "Italian"
   )
MSG_HASH(
   MSG_HASH_REICAST_DIV_MATCHING_DESC,
   "DIV Matching"
   )
MSG_HASH(
   MSG_HASH_REICAST_DIV_MATCHING_INFO,
   "Optimize integer division"
   )
MSG_HASH(
   MSG_HASH_REICAST_FORCE_WINCE_DESC,
   "Force Windows CE Mode"
   )
MSG_HASH(
   MSG_HASH_REICAST_FORCE_WINCE_INFO,
   "Enable full MMU emulation and other settings for Windows CE games"
   )
MSG_HASH(
   MSG_HASH_REICAST_ANALOG_STICK_DEADZONE_DESC,
   "Analog Stick Deadzone"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_0,
   "0%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_5,
   "5%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_10,
   "10%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_15,
   "15%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_20,
   "20%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_25,
   "25%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_30,
   "30%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_40,
   "40%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_50,
   "50%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_60,
   "60%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_70,
   "70%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_80,
   "80%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_90,
   "90%"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_100,
   "100%"
   )
MSG_HASH(
   MSG_HASH_REICAST_TRIGGER_DEADZONE_DESC,
   "Trigger Deadzone"
   )
MSG_HASH(
   MSG_HASH_REICAST_DIGITAL_TRIGGERS_DESC,
   "Digital Triggers"
   )
MSG_HASH(
   MSG_HASH_REICAST_ENABLE_DSP_DESC,
   "Enable DSP"
   )
MSG_HASH(
   MSG_HASH_REICAST_ENABLE_DSP_INFO,
   "Enable emulation of the Dreamcast's audio DSP (digital signal processor). Improves the accuracy of generated sound, but increases performance requirements."
   )
MSG_HASH(
   MSG_HASH_REICAST_ANISOTROPIC_FILTERING_DESC,
   "Anisotropic Filtering"
   )
MSG_HASH(
   MSG_HASH_REICAST_ANISOTROPIC_FILTERING_INFO,
   "Enhance the quality of textures on surfaces that are at oblique viewing angles with respect to the camera."
   )
MSG_HASH(
   MSG_HASH_REICAST_PVR2_FILTERING_DESC,
   "PowerVR2 Post-processing Filter"
   )
MSG_HASH(
   MSG_HASH_REICAST_PVR2_FILTERING_INFO,
   "Post-process the rendered image to simulate effects specific to the PowerVR2 GPU and analog video signals."
   )
#ifdef HAVE_TEXUPSCALE
MSG_HASH(
   MSG_HASH_REICAST_TEXUPSCALE_DESC,
   "Texture Upscaling (xBRZ)"
   )
MSG_HASH(
   MSG_HASH_REICAST_TEXUPSCALE_INFO,
   "Enhance hand-drawn 2D pixel art graphics. Should only be used with 2D pixelized games."
   )
MSG_HASH(
   MSG_HASH_REICAST_TEXUPSCALE_MAX_FILTERED_TEXTURE_SIZE_DESC,
   "Texture Upscaling Max. Filtered Size"
   )
#endif
MSG_HASH(
   MSG_HASH_REICAST_ENABLE_RTTB_DESC,
   "Enable RTT (Render To Texture) Buffer"
   )
MSG_HASH(
   MSG_HASH_REICAST_RENDER_TO_TEXTURE_UPSCALING_DESC,
   "Render To Texture Upscaling"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_1X,
   "1x"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_2X,
   "2x"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_3X,
   "3x"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_4X,
   "4x"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_5X,
   "5x"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_6X,
   "6x"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_8X,
   "8x"
   )
#if !defined(TARGET_NO_THREADS)
MSG_HASH(
   MSG_HASH_REICAST_THREADED_RENDERING_DESC,
   "Threaded Rendering (Restart Required)"
   )
MSG_HASH(
   MSG_HASH_REICAST_THREADED_RENDERING_INFO,
   "Runs the GPU and CPU on different threads. Highly recommended."
   )
MSG_HASH(
   MSG_HASH_REICAST_SYNCHRONOUS_RENDERING_DESC,
   "Synchronous Rendering"
   )
MSG_HASH(
   MSG_HASH_REICAST_SYNCHRONOUS_RENDERING_INFO,
   "Waits for the GPU to finish rendering the previous frame instead of dropping the current one. Note: This setting only applies when 'Threaded Rendering' is enabled."
   )
MSG_HASH(
   MSG_HASH_REICAST_DELAY_FRAME_SWAPPING_DESC,
   "Delay Frame Swapping"
   )
MSG_HASH(
   MSG_HASH_REICAST_DELAY_FRAME_SWAPPING_INFO,
   "Useful to avoid flashing screens or glitchy videos. Not recommended on slow platforms. Note: This setting only applies when 'Threaded Rendering' is enabled."
   )
#endif
MSG_HASH(
   MSG_HASH_REICAST_FRAME_SKIPPING_DESC,
   "Frame Skipping"
   )
MSG_HASH(
   MSG_HASH_REICAST_FRAME_SKIPPING_INFO,
   "Sets the number of frames to skip between each displayed frame."
   )
MSG_HASH(
   MSG_HASH_REICAST_ENABLE_PURUPURU_DESC,
   "Purupuru Pack/Vibration Pack"
   )
MSG_HASH(
   MSG_HASH_REICAST_ENABLE_PURUPURU_INFO,
   "Enables controller force feedback."
   )
MSG_HASH(
   MSG_HASH_REICAST_ALLOW_SERVICE_BUTTONS_DESC,
   "Allow NAOMI Service Buttons"
   )
MSG_HASH(
   MSG_HASH_REICAST_ALLOW_SERVICE_BUTTONS_INFO,
   "Enables SERVICE button for NAOMI, to enter cabinet settings."
   )
MSG_HASH(
   MSG_HASH_REICAST_ENABLE_NAOMI_15KHZ_DIPSWITCH_DESC,
   "Enable NAOMI 15KHz Dipswitch"
   )
MSG_HASH(
   MSG_HASH_REICAST_ENABLE_NAOMI_15KHZ_DIPSWITCH_INFO,
   "This can force display in 240p, 480i or no effect at all depending on the game."
   )
MSG_HASH(
   MSG_HASH_REICAST_CUSTOM_TEXTURES_DESC,
   "Load Custom Textures"
   )
MSG_HASH(
   MSG_HASH_REICAST_DUMP_TEXTURES_DESC,
   "Dump Textures"
   )
MSG_HASH(
   MSG_HASH_REICAST_PER_CONTENT_VMUS_DESC,
   "Per-Game VMUs"
   )
MSG_HASH(
   MSG_HASH_REICAST_PER_CONTENT_VMUS_INFO,
   "When disabled, all games share 4 VMU save files (A1, B1, C1, D1) located in RetroArch's system directory. The 'VMU A1' setting creates a unique VMU 'A1' file in RetroArch's save directory for each game that is launched. The 'All VMUs' setting creates 4 unique VMU files (A1, B1, C1, D1) for each game that is launched."
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_VMU_A1,
   "VMU A1"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_ALL_VMUS,
   "All VMUs"
   )
MSG_HASH(
   MSG_HASH_REICAST_SHOW_VMU_SCREEN_SETTINGS_DESC,
   "Show VMU Display Settings"
   )
MSG_HASH(
   MSG_HASH_REICAST_SHOW_VMU_SCREEN_SETTINGS_INFO,
   "Enable configuration of emulated VMU LCD screen visibility, size, position and color. Note: Quick Menu must be toggled for this setting to take effect."
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU1_SCREEN_DISPLAY_DESC,
   "VMU Screen 1 Display"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU1_SCREEN_POSITION_DESC,
   "VMU Screen 1 Position"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_UPPER_LEFT,
   "Upper Left"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_UPPER_RIGHT,
   "Upper Right"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LOWER_LEFT,
   "Lower Left"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LOWER_RIGHT,
   "Lower Right"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU1_SCREEN_SIZE_MULT_DESC,
   "VMU Screen 1 Size"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU1_PIXEL_ON_COLOR_DESC,
   "VMU Screen 1 Pixel On Color"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_DEFAULT_ON,
   "Default ON"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_DEFAULT_OFF,
   "Default OFF"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_BLACK,
   "Black"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_BLUE,
   "Blue"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_BLUE,
   "Light Blue"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_GREEN,
   "Green"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_CYAN,
   "Cyan"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_CYAN_BLUE,
   "Cyan Blue"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_GREEN,
   "Light Green"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_CYAN_GREEN,
   "Cyan Green"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_CYAN,
   "Light Cyan"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_RED,
   "Red"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_PURPLE,
   "Purple"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_PURPLE,
   "Light Purple"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_YELLOW,
   "Yellow"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_GRAY,
   "Gray"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_PURPLE_2,
   "Light Purple (2)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_GREEN_2,
   "Light Green (2)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_GREEN_3,
   "Light Green (3)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_CYAN_2,
   "Light Cyan (2)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_RED_2,
   "Light Red (2)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_MAGENTA,
   "Magenta"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_ORANGE,
   "Light Orange"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_ORANGE,
   "Orange"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_PURPLE_3,
   "Light Purple (3)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_YELLOW,
   "Light Yellow"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_LIGHT_YELLOW_2,
   "Light Yellow (2)"
   )
MSG_HASH(
   MSG_HASH_OPTION_VAL_WHITE,
   "White"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU1_PIXEL_OFF_COLOR_DESC,
   "VMU Screen 1 Pixel Off Color"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU1_SCREEN_OPACITY_DESC,
   "VMU Screen 1 Opacity"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU2_SCREEN_DISPLAY_DESC,
   "VMU Screen 2 Display"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU2_SCREEN_POSITION_DESC,
   "VMU Screen 2 Position"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU2_SCREEN_SIZE_MULT_DESC,
   "VMU Screen 2 Size"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU2_PIXEL_ON_COLOR_DESC,
   "VMU Screen 2 Pixel On Color"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU2_PIXEL_OFF_COLOR_DESC,
   "VMU Screen 2 Pixel Off Color"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU2_SCREEN_OPACITY_DESC,
   "VMU Screen 2 Opacity"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU3_SCREEN_DISPLAY_DESC,
   "VMU Screen 3 Display"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU3_SCREEN_POSITION_DESC,
   "VMU Screen 3 Position"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU3_SCREEN_SIZE_MULT_DESC,
   "VMU Screen 3 Size"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU3_PIXEL_ON_COLOR_DESC,
   "VMU Screen 3 Pixel On Color"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU3_PIXEL_OFF_COLOR_DESC,
   "VMU Screen 3 Pixel Off Color"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU3_SCREEN_OPACITY_DESC,
   "VMU Screen 3 Opacity"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU4_SCREEN_DISPLAY_DESC,
   "VMU Screen 4 Display"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU4_SCREEN_POSITION_DESC,
   "VMU Screen 4 Position"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU4_SCREEN_SIZE_MULT_DESC,
   "VMU Screen 4 Size"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU4_PIXEL_ON_COLOR_DESC,
   "VMU Screen 4 Pixel On Color"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU4_PIXEL_OFF_COLOR_DESC,
   "VMU Screen 4 Pixel Off Color"
   )
MSG_HASH(
   MSG_HASH_REICAST_VMU4_SCREEN_OPACITY_DESC,
   "VMU Screen 4 Opacity"
   )
MSG_HASH(
   MSG_HASH_REICAST_SHOW_LIGHTGUN_SETTINGS_DESC,
   "Show Light Gun Settings"
   )
MSG_HASH(
   MSG_HASH_REICAST_SHOW_LIGHTGUN_SETTINGS_INFO,
   "Enable configuration of light gun crosshair display options. Note: Quick Menu must be toggled for this setting to take effect."
   )
MSG_HASH(
   MSG_HASH_REICAST_LIGHTGUN1_CROSSHAIR_DESC,
   "Gun Crosshair 1 Display"
   )
MSG_HASH(
   MSG_HASH_REICAST_LIGHTGUN2_CROSSHAIR_DESC,
   "Gun Crosshair 2 Display"
   )
MSG_HASH(
   MSG_HASH_REICAST_LIGHTGUN3_CROSSHAIR_DESC,
   "Gun Crosshair 3 Display"
   )
MSG_HASH(
   MSG_HASH_REICAST_LIGHTGUN4_CROSSHAIR_DESC,
   "Gun Crosshair 4 Display"
   )
