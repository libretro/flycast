#!/bin/sh
# Threaded rendering, run for real.
#
#   sh tools/threads/live.sh [path/to/flycast_libretro.so]
#
# Boots a small test disc (live_disc.py, live_prog.c) in RetroArch with
# Threaded Rendering on, on an Xvfb display, and checks the three things
# the emulation thread's handshake with the frontend has to get right:
#
#   1. content runs and closes with a save state made on the way out, the
#      save immediately followed by the unload;
#      a screenshot taken on that way out has to show the colour the
#      disc painted its background texture last, so a write to a texture
#      in video memory is seen to reach the screen;
#   2. that state is loaded again as the content starts;
#   3. save state, load state and reset, sent one right after the other
#      for as long as the content runs, with states saved from the
#      frontend's task thread. None of them may fail.
#
# A handshake that loses a request shows up as RetroArch not exiting,
# which the timeout turns into a failure.
#
# Before those, a build of the disc that renders without ever writing a
# region array is run for a few seconds: the render thread has to get
# through frames like that instead of walking video memory for ever.
#
# After them the disc is run once with Threaded Rendering off, where one
# thread does everything, and the screenshot is checked again. That run and
# the first one have then made the same 300 frames from the same start, so
# the screenshots they take have to be the same byte for byte, and so do
# the save states they leave, bar the host addresses a state carries
# (live_same.py; a second non-threaded run shows where those are).
# Threaded rendering makes the frame non-threaded rendering makes; it only
# makes it on two threads.
#
# All of it is done once per video driver in $DRIVERS (default: "gl
# vulkan"), which is what picks the core's OpenGL or Vulkan renderer.
#
# Last, for each driver in $RING_DRIVERS (default: "gl glcore vulkan"), the
# disc is run with RetroArch's own threaded video on. That gives a
# hardware-rendered core a different framebuffer to draw into every frame,
# a ring of three, so the screenshot is checked after 300, 301 and 302
# frames: one for each of them. A core that keeps drawing into the
# framebuffer it was given first shows the right picture one frame in
# three.
#
# Then the per-pixel renderers, which are different renderers with their own
# shaders and passes: for each driver in $PIXEL_DRIVERS (default: "glcore
# vulkan")
# the disc is run with Alpha Sorting set to per-pixel and the screenshot is
# checked. The log has to say the per-pixel renderer was the one created:
# the core falls back to the per-triangle one when it cannot have it, and
# a test of the wrong renderer passes just as well. The OpenGL one needs
# OpenGL 4.3 from the frontend's driver; take "glcore" out where there is
# none.
#
# Then the program without its disc: live_disc.py writes it as an ELF file,
# and the core is given that as its content. The HLE BIOS has to load it
# and start it with the drive empty; the program finds no disc to read and
# says so with a blue background, which live_shot.py --no-disc expects.
# Three damaged copies of the file follow, each of which has to be refused
# without taking the core down. $ELF_DRIVER is the video driver for this
# (default: "gl"); empty leaves it out.
#
# Last, the sound. The disc plays its audio track, looping, through the
# sound chip at full level. RetroArch is given audio_tap.c to load in place
# of the core: it loads the core, passes everything through, and writes
# down every sample the core sends on the way. live_audio.py checks that
# against the track: after a moment of silence it has to be the track,
# sample for sample, round and round. Then once more with a state saved
# and loaded every second or so while it plays: that run has to be the
# first with stretches of it heard again, going on from every load exactly
# as the first run did from that point. $SOUND_DRIVER is the video driver
# this runs with (default: "gl"); empty leaves the pass out.
#
# Needs python3, a C compiler, xvfb-run, RetroArch ($RETROARCH, default: retroarch) and
# a GL and a Vulkan driver; Mesa's software ones will do. Fails rather
# than skips when one is missing. Uses UDP port 55355.
set -e
# The checkers import one another; Python is not to leave compiled copies of
# them in the tree
PYTHONDONTWRITEBYTECODE=1
export PYTHONDONTWRITEBYTECODE
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORE=${1:-$ROOT/flycast_libretro.so}
RETROARCH=${RETROARCH:-retroarch}
DRIVERS=${DRIVERS:-gl vulkan}
RING_DRIVERS=${RING_DRIVERS:-gl glcore vulkan}
PIXEL_DRIVERS=${PIXEL_DRIVERS:-glcore vulkan}
SOUND_DRIVER=${SOUND_DRIVER-gl}
ELF_DRIVER=${ELF_DRIVER-gl}

command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }
command -v xvfb-run >/dev/null || { echo "xvfb-run not found" >&2; exit 1; }
command -v "$RETROARCH" >/dev/null || { echo "$RETROARCH not found" >&2; exit 1; }
[ -f "$CORE" ] || { echo "$CORE not found: build the core first" >&2; exit 1; }

WORK=${TMPDIR:-/tmp}/threads_live.$$
mkdir -p "$WORK/system/dc" "$WORK/saves" "$WORK/states"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/bare"
python3 "$ROOT/tools/threads/live_disc.py" "$WORK/test.gdi"
python3 "$ROOT/tools/threads/live_disc.py" "$WORK/bare/bare.gdi" --no-region-array

cat > "$WORK/ra.cfg" <<CFG
audio_driver = "null"
audio_enable = "false"
input_driver = "null"
input_joypad_driver = "null"
video_threaded = "false"
video_vsync = "false"
video_fullscreen = "false"
pause_nonactive = "false"
config_save_on_exit = "false"
gamemode_enable = "false"
system_directory = "$WORK/system"
savefile_directory = "$WORK/saves"
savestate_directory = "$WORK/states"
core_options_path = "$WORK/core-options.cfg"
global_core_options = "true"
savestate_auto_save = "true"
savestate_auto_load = "true"
savestate_file_compression = "false"
network_cmd_enable = "true"
network_cmd_port = "55355"
CFG
cat > "$WORK/core-options.cfg" <<CFG
reicast_threaded_rendering = "enabled"
reicast_hle_bios = "enabled"
reicast_vmu1_screen_display = "enabled"
CFG
cat > "$WORK/core-options-pixel.cfg" <<CFG
reicast_threaded_rendering = "enabled"
reicast_hle_bios = "enabled"
reicast_vmu1_screen_display = "enabled"
reicast_alpha_sorting = "per-pixel (accurate)"
CFG
cat > "$WORK/core-options-off.cfg" <<CFG
reicast_threaded_rendering = "disabled"
reicast_hle_bios = "enabled"
reicast_vmu1_screen_display = "enabled"
CFG

# With VALIDATE=1 every run is made with the Vulkan validation layer on
# (VK_LAYER_KHRONOS_validation has to be installed), and anything it
# reports as an error fails the test.
if [ -n "$VALIDATE" ]; then
   cat > "$WORK/vk_layer_settings.txt" <<CFG
khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG
khronos_validation.log_filename = $WORK/validation.txt
khronos_validation.report_flags = error
CFG
   VK_LAYER_SETTINGS_PATH="$WORK/vk_layer_settings.txt"
   VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
   export VK_LAYER_SETTINGS_PATH VK_INSTANCE_LAYERS
fi

# run <log> <frames> [disc] [screenshot]
run() {
   rm -f "$WORK/validation.txt"
   LIBGL_ALWAYS_SOFTWARE=1 timeout 300 xvfb-run -a -s "-screen 0 800x600x24" \
      "$RETROARCH" --config "$WORK/ra.cfg" --appendconfig "$WORK/driver.cfg" \
      -L "$CORE" "${3:-$WORK/test.gdi}" \
      --max-frames="$2" ${4:+--max-frames-ss --max-frames-ss-path="$4"} \
      --verbose > "$WORK/$1" 2>&1 || {
         echo "FAIL: RetroArch did not exit cleanly ($1)" >&2
         tail -n 20 "$WORK/$1" >&2
         exit 1
      }
   if [ -s "$WORK/validation.txt" ]; then
      echo "FAIL: the Vulkan validation layer reported errors ($1)" >&2
      head -c 2000 "$WORK/validation.txt" >&2
      exit 1
   fi
}

expect() {
   grep -q "$2" "$WORK/$1" || {
      echo "FAIL: '$2' not in $1" >&2
      tail -n 20 "$WORK/$1" >&2
      exit 1
   }
}

for DRV in $DRIVERS; do
   case $DRV in
      vulkan) CONTEXT="Requesting Vulkan context" ;;
      *)      CONTEXT="Requesting OpenGL context" ;;
   esac
   echo "video_driver = \"$DRV\"" > "$WORK/driver.cfg"
   rm -rf "$WORK/states" "$WORK/saves"
   mkdir -p "$WORK/states" "$WORK/saves"

   echo "== $DRV: render without a region array"
   run $DRV-bare.log 200 "$WORK/bare/bare.gdi"
   expect $DRV-bare.log "$CONTEXT"

   echo "== $DRV: run, save a state on the way out"
   run $DRV-first.log 300 "$WORK/test.gdi" "$WORK/$DRV.png"
   expect $DRV-first.log "Auto save state to .* succeeded"
   expect $DRV-first.log "$CONTEXT"
   python3 "$ROOT/tools/threads/live_shot.py" "$WORK/$DRV.png" || {
      echo "FAIL: the screen does not show the last texture written" >&2
      exit 1
   }
   cp "$WORK"/states/*/test.state.auto "$WORK/$DRV-on.state"

   echo "== $DRV: load it at startup, then save, load and reset while running"
   # Give slot 0 a state to begin with: the first command to get through
   # may be a load.
   for f in "$WORK"/states/*/test.state.auto; do
      cp "$f" "${f%.auto}"
   done
   python3 - <<'PY' &
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
while True:
    for cmd in (b"SAVE_STATE", b"LOAD_STATE", b"SAVE_STATE", b"RESET", b"LOAD_STATE"):
        s.sendto(cmd, ("127.0.0.1", 55355))
        time.sleep(0.05)
    # The lid opened, a reset with it open, and the lid closed again,
    # well apart: two toggles in one frame fold into one, and the lid is
    # to stay open for a few sectors of the audio track that is playing.
    for cmd in (b"DISK_EJECT_TOGGLE", b"RESET", b"DISK_EJECT_TOGGLE"):
        s.sendto(cmd, ("127.0.0.1", 55355))
        time.sleep(0.3)
PY
   POKE=$!
   trap 'kill $POKE 2>/dev/null || true; rm -rf "$WORK"' EXIT
   run $DRV-second.log 600
   kill $POKE 2>/dev/null || true
   expect $DRV-second.log "Auto-loading save state from .* succeeded"
   expect $DRV-second.log "Auto save state to .* succeeded"
   if grep -qiE "failed to (save|load) state|could not serialize" "$WORK/$DRV-second.log"; then
      echo "FAIL: a save or load failed" >&2
      grep -iE "failed to (save|load) state|could not serialize" "$WORK/$DRV-second.log" | head -n 5 >&2
      exit 1
   fi
   LOADS=$(grep -c 'Loading state' "$WORK/$DRV-second.log" || true)
   RESETS=$(grep -c '\[Core\] Reset' "$WORK/$DRV-second.log" || true)
   EJECTS=$(grep -c 'Ejected virtual disc tray' "$WORK/$DRV-second.log" || true)
   echo "loads: $LOADS, resets: $RESETS, ejects: $EJECTS"
   if [ "$LOADS" -lt 5 ] || [ "$RESETS" -lt 5 ] || [ "$EJECTS" -lt 5 ]; then
      echo "FAIL: the commands did not reach RetroArch" >&2
      exit 1
   fi

   echo "== $DRV: threaded rendering off"
   echo "core_options_path = \"$WORK/core-options-off.cfg\"" >> "$WORK/driver.cfg"
   rm -rf "$WORK/states" "$WORK/saves"
   mkdir -p "$WORK/states" "$WORK/saves"
   run $DRV-off.log 300 "$WORK/test.gdi" "$WORK/$DRV-off.png"
   expect $DRV-off.log "$CONTEXT"
   expect $DRV-off.log "core options file to .*core-options-off.cfg"
   python3 "$ROOT/tools/threads/live_shot.py" "$WORK/$DRV-off.png" || {
      echo "FAIL: the screen does not show the last texture written" >&2
      exit 1
   }
   expect $DRV-off.log "Auto save state to .* succeeded"
   cp "$WORK"/states/*/test.state.auto "$WORK/$DRV-off.state"
   cmp "$WORK/$DRV.png" "$WORK/$DRV-off.png" || {
      echo "FAIL: threaded and non-threaded rendering drew different frames" >&2
      exit 1
   }

   echo "== $DRV: threaded rendering off, once more, to compare save states"
   rm -rf "$WORK/states" "$WORK/saves"
   mkdir -p "$WORK/states" "$WORK/saves"
   run $DRV-off2.log 300
   python3 "$ROOT/tools/threads/live_same.py" "$WORK/$DRV-on.state" \
      "$WORK/$DRV-off.state" "$WORK"/states/*/test.state.auto || {
      echo "FAIL: threaded and non-threaded rendering left different save states" >&2
      exit 1
   }
done
for DRV in $RING_DRIVERS; do
   echo "== $DRV: the frontend's threaded video"
   printf 'video_driver = "%s"\nvideo_threaded = "true"\n' "$DRV" > "$WORK/driver.cfg"
   for FRAMES in 300 301 302; do
      rm -rf "$WORK/states" "$WORK/saves"
      mkdir -p "$WORK/states" "$WORK/saves"
      run $DRV-ring-$FRAMES.log $FRAMES "$WORK/test.gdi" "$WORK/$DRV-ring-$FRAMES.png"
      python3 "$ROOT/tools/threads/live_shot.py" "$WORK/$DRV-ring-$FRAMES.png" || {
         echo "FAIL: wrong picture after $FRAMES frames with threaded video" >&2
         exit 1
      }
   done
   expect $DRV-ring-302.log "Starting threaded video driver"
done
for DRV in $PIXEL_DRIVERS; do
   case $DRV in
      vulkan) CREATED="Creating Vulkan per-pixel renderer" ;;
      *)      CREATED="Creating Open GL per-pixel renderer" ;;
   esac
   echo "== $DRV: the per-pixel renderer"
   echo "video_driver = \"$DRV\"" > "$WORK/driver.cfg"
   echo "core_options_path = \"$WORK/core-options-pixel.cfg\"" >> "$WORK/driver.cfg"
   rm -rf "$WORK/states" "$WORK/saves"
   mkdir -p "$WORK/states" "$WORK/saves"
   run $DRV-pixel.log 300 "$WORK/test.gdi" "$WORK/$DRV-pixel.png"
   expect $DRV-pixel.log "core options file to .*core-options-pixel.cfg"
   expect $DRV-pixel.log "$CREATED"
   # ...and that it stayed: the OpenGL one hands over to the per-triangle
   # renderer when the context it is given is older than OpenGL 4.3
   if grep -aq "doesn't support per-pixel sorting" "$WORK/$DRV-pixel.log"; then
      echo "FAIL: the per-pixel renderer gave way to the per-triangle one ($DRV-pixel.log)" >&2
      exit 1
   fi
   python3 "$ROOT/tools/threads/live_shot.py" --per-pixel "$WORK/$DRV-pixel.png" || {
      echo "FAIL: wrong picture from the per-pixel renderer" >&2
      exit 1
   }
done

if [ -n "$ELF_DRIVER" ]; then
   echo "== $ELF_DRIVER: the program as an ELF file, no disc"
   python3 "$ROOT/tools/threads/live_disc.py" "$WORK/prog.elf"
   echo "video_driver = \"$ELF_DRIVER\"" > "$WORK/driver.cfg"
   rm -rf "$WORK/states" "$WORK/saves"
   mkdir -p "$WORK/states" "$WORK/saves"
   run elf.log 300 "$WORK/prog.elf" "$WORK/elf.png"
   python3 "$ROOT/tools/threads/live_shot.py" --no-disc "$WORK/elf.png" || {
      echo "FAIL: wrong picture from the program started as an ELF" >&2
      exit 1
   }
   # ...and files that are not what they say they are: one cut off inside
   # its header, one whose segment lies outside it, one whose segment runs
   # off the end of memory. Each has to be refused and leave the core up.
   python3 - "$WORK" <<'PY'
import struct, sys
d = open(sys.argv[1] + "/prog.elf", "rb").read()
open(sys.argv[1] + "/short.elf", "wb").write(d[:20])
for name, at, value in (("outside", 52 + 4, 0x7FFFFFF0), ("offend", 52 + 8, 0x8CFFFF00)):
    b = bytearray(d)
    struct.pack_into("<I", b, at, value)
    open(sys.argv[1] + "/" + name + ".elf", "wb").write(b)
PY
   for BAD in short outside offend; do
      run elf-$BAD.log 60 "$WORK/$BAD.elf"
      expect elf-$BAD.log "Failed to open .*$BAD.elf"
   done
fi

if [ -n "$SOUND_DRIVER" ]; then
   echo "== $SOUND_DRIVER: the sound"
   "${CC:-cc}" -O1 -shared -fPIC -o "$WORK/audio_tap.so" \
      "$ROOT/tools/threads/audio_tap.c" -ldl
   echo "video_driver = \"$SOUND_DRIVER\"" > "$WORK/driver.cfg"
   rm -rf "$WORK/states" "$WORK/saves"
   mkdir -p "$WORK/states" "$WORK/saves"
   # RetroArch loads the tap, and the tap loads the core
   AUDIO_TAP_CORE=$CORE
   AUDIO_TAP_OUT=$WORK/sound.pcm
   export AUDIO_TAP_CORE AUDIO_TAP_OUT
   CORE=$WORK/audio_tap.so
   run sound.log 300
   python3 "$ROOT/tools/threads/live_audio.py" "$WORK/sound.pcm" || {
      echo "FAIL: wrong sound" >&2
      exit 1
   }

   echo "== $SOUND_DRIVER: the sound, with states saved and loaded while it plays"
   rm -rf "$WORK/states" "$WORK/saves"
   mkdir -p "$WORK/states" "$WORK/saves"
   python3 - <<'PY' &
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
time.sleep(3)
while True:
    s.sendto(b"SAVE_STATE", ("127.0.0.1", 55355))
    time.sleep(0.4)
    s.sendto(b"LOAD_STATE", ("127.0.0.1", 55355))
    time.sleep(0.8)
PY
   POKE=$!
   trap 'kill $POKE 2>/dev/null || true; rm -rf "$WORK"' EXIT
   AUDIO_TAP_OUT=$WORK/sound-reloaded.pcm
   run sound-reloaded.log 300
   kill $POKE 2>/dev/null || true
   CORE=$AUDIO_TAP_CORE
   python3 "$ROOT/tools/threads/live_audio.py" --reloaded "$WORK/sound.pcm" \
      "$WORK/sound-reloaded.pcm" || {
      echo "FAIL: the sound did not go on from a loaded state as it had from there" >&2
      exit 1
   }

   # The SH4 Timing core option's other setting. The disc times its frames
   # by the processor's own timer and keys its sound by the frame, so both
   # checks say whether time still passes as it should; and its wait for
   # scanline 16 is a loop of the kind the x86-64 recompiler gives up the
   # rest of a time slice in when it sees one.
   echo "== $SOUND_DRIVER: picture and sound under the accurate SH4 timing"
   cat > "$WORK/core-options-accurate.cfg" <<CFG
reicast_threaded_rendering = "enabled"
reicast_hle_bios = "enabled"
reicast_vmu1_screen_display = "enabled"
reicast_sh4_timing = "accurate"
CFG
   echo "core_options_path = \"$WORK/core-options-accurate.cfg\"" >> "$WORK/driver.cfg"
   rm -rf "$WORK/states" "$WORK/saves"
   mkdir -p "$WORK/states" "$WORK/saves"
   AUDIO_TAP_OUT=$WORK/sound-accurate.pcm
   CORE=$WORK/audio_tap.so
   run sound-accurate.log 300 "$WORK/test.gdi" "$WORK/accurate.png"
   CORE=$AUDIO_TAP_CORE
   expect sound-accurate.log "SH4 timing: accurate"
   python3 "$ROOT/tools/threads/live_shot.py" "$WORK/accurate.png" || {
      echo "FAIL: wrong picture under the accurate SH4 timing" >&2
      exit 1
   }
   python3 "$ROOT/tools/threads/live_audio.py" "$WORK/sound-accurate.pcm" || {
      echo "FAIL: wrong sound under the accurate SH4 timing" >&2
      exit 1
   }
fi
echo "live threads test passed"
