parallel-dc
===========

parallel-dc is a Sega Dreamcast, NAOMI, NAOMI 2 and Atomiswave emulator
core for [libretro](https://www.libretro.com) frontends such as
[RetroArch](https://github.com/libretro/RetroArch).

It started life as libretro's own fork of Flycast (itself a fork of
reicast) and has since been developed independently. It is actively
maintained and is no longer deprecated: much of the emulator has been
audited against upstream [Flycast](https://github.com/flyinghead/flycast)
and other references, the useful upstream fixes were brought over after
being verified, and a great deal has been added or rewritten that
upstream does not have.

The same goals apply throughout the code base:

- **Accuracy** - emulate what the hardware does, not what one game needs.
- **Performance** - every change has to be at least as fast as what it
  replaces, and is measured. No allocation, shader compilation or blocking
  wait in the middle of a game.
- **No locks** - threads talk through lock-free queues and atomics
  (libretro-common's `retro_atomic`), never mutexes or sleeps.
- **Portability** - plain C/C89 where possible, built for every libretro
  platform, with libretro-common used for threads, file I/O and archives.

What it runs
------------

- **Dreamcast** discs: CHD, GDI, CUE/BIN, CDI, and ELF programs, plus
  M3U playlists for multi-disc games.
- **NAOMI**, **NAOMI 2** and **Atomiswave** games, as MAME romsets or
  flat `.bin`/`.dat` images. A romset is recognised by its contents, not
  its file name, so non-MAME sets work too, and a game can be started out
  of a merged set (a core option picks which one).
- Any of the above inside **zip, 7z or rar** archives (RAR 2.9 to 5).
  Large compressed disc tracks are read as they are needed instead of
  being unpacked into memory first.
- **No BIOS required.** The Dreamcast and NAOMI BIOS routines are
  reimplemented (HLE), including the NAOMI test menu. With
  *Use Real BIOS (If Available)* on (the default), a real BIOS in the
  system directory is used when present.

What is different from upstream Flycast
---------------------------------------

### Video

- **A renderer for every frontend driver**: OpenGL / OpenGL ES, Vulkan,
  Direct3D 9, Direct3D 11 and Direct3D 12. The core uses whichever the
  frontend prefers.
- **Per-pixel translucency sorting** (order-independent transparency) on
  OpenGL 4.3+, Vulkan, Direct3D 11 and Direct3D 12, chosen with a core
  option rather than a build flag. Frames with more translucent pixels
  than the buffers hold are drawn in bands instead of losing pixels.
- **Compute renderer** (Vulkan, experimental, *Rendering Method* option):
  draws the screen tile by tile with compute shaders the way the PowerVR2
  does, with exact per-pixel sorting. The rasterizer remains the default.
- **TA processing on the GPU** (*TA Processing* option, Vulkan and
  OpenGL 4.3+): the display lists the game sends are turned into vertices
  on the GPU, byte-identical to the CPU path.
- **SSAA** (2x/4x/8x, Vulkan), sized to the video memory the driver
  reports.
- **Texture decoding on the GPU**, with textures written straight into an
  upload ring (no staging copy per texture). Shaders and pipelines are
  built at start-up and cached on disk, so there are no mid-game compile
  hitches.
- **Render-to-texture** kept on the GPU and copied back to emulated video
  memory only when the game actually reads it; the per-game RTT setting
  is gone.
- PowerVR features are always on, as on hardware: fog, modifier volumes
  (shadows, including in the per-triangle renderers) and mipmapping.
  *Force Texture LOD0* replaces the old mipmapping switch.
- Fog, shadows, depth, palettes, texture formats and clipping audited and
  fixed against the hardware; host-side precision loss in shaders and
  colour conversion removed. glm replaced by a small C89 matrix header.
- Vulkan memory handled by a lean C89 block heap instead of the Vulkan
  Memory Allocator, sized from the driver's real memory budget.
- Threaded rendering reworked: one emulated frame per `retro_run`, no
  skipped frames, output identical to non-threaded rendering.

### CPU and timing

- **SH4**: instruction fixes from an audit against upstream and the
  hardware (DIV1, FTRC, FIPR/FTRV and more); every instruction now has a
  recompiled form; faster x86-64 and ARM64 recompilers (direct block
  links, loads/stores through the host memory mapping, fewer instructions
  per access); and an optional cycle-accurate timing model
  (*SH4 Timing*: Legacy / Accurate; NAOMI 2 always uses Accurate).
- **MMU** (Windows CE games): supported by the 32-bit ARM recompiler as
  well, with translation tables on every host, correct TLB invalidation,
  write protection and self-modifying-code handling.
- **ARM7 recompiler** for x86-64 (*Sound CPU Recompiler* option), with an
  interpreter fallback.
- Scanline and frame timing fixed so frames come out at the right rate
  without lost lines.

### NAOMI 2

- The ELAN geometry processor is emulated in C89, with SIMD (SSE2/NEON)
  vertex lighting and its memory mapped for the recompilers.
- 45 NAOMI 2 games in the game table, with input maps; romsets and
  cartridge sets start without a BIOS.

### Sound

- AICA audited: two-pole low-pass filter, 20-bit interpolation, faster
  channel loop, save states that keep pending notes and CD audio.
- The DSP is always emulated. Each video frame carries exactly the number
  of samples the hardware produces for it, not fixed-size chunks.

### Frontend and I/O

- Lock-free threading throughout: emulation thread, CHD read-ahead,
  modem / broadband adapter / NAOMI network, save writer, archive
  decoding. No C++11 threads, `<mutex>` or `<chrono>`.
- File I/O goes through the frontend's VFS and libretro-common, with
  sectors read in place rather than copied and archives decoded in the
  background.
- Obsolete core options removed (Framerate, Fog Effects, Volume Modifier,
  Enable DSP, Enable RTT Buffer, the OSD/FPS options and others): the
  hardware behaviour they switched off is now always emulated.

Building
--------

```
make -j$(nproc)
```

Use `platform=<name>` for other targets (for example `platform=win`,
`platform=libnx`, `platform=emscripten`; see the Makefile). The core is
built as `flycast_libretro`, and its options keep the `reicast_` prefix so
existing configurations still apply.

Tests
-----

`tools/` holds the regression tests used during development: a headless
runner and test disc (`tools/threads`), and checks for the SH4 and ARM7
recompilers, AICA/DSP, the NAOMI 2 geometry chip, the tile accelerator on
CPU and GPU, the texture decoders on every API, a reference software
renderer used as an oracle (`tools/rend/refsw`), the Vulkan heap, and the
CHD and archive readers.

License
-------

GPL version 2; see [LICENSE](LICENSE).

All code contributed to this fork is *not* bound by the Individual
Contributor License Agreement of the reicast repository
(https://github.com/reicast/reicast-emulator) and shall *not* be
considered a contribution to that repository.
