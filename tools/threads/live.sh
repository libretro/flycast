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
#   2. that state is loaded again as the content starts;
#   3. save state, load state and reset, sent one right after the other
#      for as long as the content runs, with states saved from the
#      frontend's task thread. None of them may fail.
#
# A handshake that loses a request shows up as RetroArch not exiting,
# which the timeout turns into a failure.
#
# All of it is done once per video driver in $DRIVERS (default: "gl
# vulkan"), which is what picks the core's OpenGL or Vulkan renderer.
#
# Needs python3, xvfb-run, RetroArch ($RETROARCH, default: retroarch) and
# a GL and a Vulkan driver; Mesa's software ones will do. Fails rather
# than skips when one is missing. Uses UDP port 55355.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CORE=${1:-$ROOT/flycast_libretro.so}
RETROARCH=${RETROARCH:-retroarch}
DRIVERS=${DRIVERS:-gl vulkan}

command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }
command -v xvfb-run >/dev/null || { echo "xvfb-run not found" >&2; exit 1; }
command -v "$RETROARCH" >/dev/null || { echo "$RETROARCH not found" >&2; exit 1; }
[ -f "$CORE" ] || { echo "$CORE not found: build the core first" >&2; exit 1; }

WORK=${TMPDIR:-/tmp}/threads_live.$$
mkdir -p "$WORK/system/dc" "$WORK/saves" "$WORK/states"
trap 'rm -rf "$WORK"' EXIT

python3 "$ROOT/tools/threads/live_disc.py" "$WORK/test.gdi"

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
network_cmd_enable = "true"
network_cmd_port = "55355"
CFG
cat > "$WORK/core-options.cfg" <<CFG
reicast_threaded_rendering = "enabled"
reicast_hle_bios = "enabled"
CFG

# run <log> <frames>
run() {
   LIBGL_ALWAYS_SOFTWARE=1 timeout 300 xvfb-run -a -s "-screen 0 800x600x24" \
      "$RETROARCH" --config "$WORK/ra.cfg" --appendconfig "$WORK/driver.cfg" \
      -L "$CORE" "$WORK/test.gdi" \
      --max-frames="$2" --verbose > "$WORK/$1" 2>&1 || {
         echo "FAIL: RetroArch did not exit cleanly ($1)" >&2
         tail -n 20 "$WORK/$1" >&2
         exit 1
      }
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

   echo "== $DRV: run, save a state on the way out"
   run $DRV-first.log 300
   expect $DRV-first.log "Auto save state to .* succeeded"
   expect $DRV-first.log "$CONTEXT"

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
   echo "loads: $LOADS, resets: $RESETS"
   if [ "$LOADS" -lt 5 ] || [ "$RESETS" -lt 5 ]; then
      echo "FAIL: the commands did not reach RetroArch" >&2
      exit 1
   fi
done
echo "live threads test passed"
