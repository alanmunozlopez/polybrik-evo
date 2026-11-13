#!/bin/bash
# Knulli (Batocera-based) launcher for PolyBrik on the TrimUI Brick Pro. Tested on a Brick Pro with Knulli
# Scarab 2026-08-19: sound, USB MIDI, the button map and a clean return to the menu. The log is
# PolyBrik/PolyBrik.txt.
#
# Install: copy this file and the PolyBrik folder next to it (tracker-sdl, launch.sh, content/,
# content.version) to /userdata/roms/ports/, then update the game list in the Ports system.
# PolyBrik keeps its projects in /userdata/PolyBrik, as it keeps them in /mnt/SDCARD/PolyBrik on NextUI.
#
# What this adds to launch.sh (the NextUI launcher, shared, unchanged for NextUI):
# - SDCARD_PATH and LOGS_PATH for Knulli.
# - DEVICE: from /userdata/PolyBrik/device if you create it (one word), else brickpro-knulli when
#   Knulli's "TRIMUI Brick Pro Controller" is present (Knulli numbers the pad's buttons differently from
#   NextUI), else brickpro.
# - The codec: PipeWire keeps the Brick's sound card (hw:0,0) open while the menu runs. PolyBrik is a
#   static binary and talks to ALSA directly, so the audio service is stopped while PolyBrik runs and
#   started again through Knulli's own init script, which sets up D-Bus and waits for the sink, so the
#   menu gets its sound back.
# - The framebuffer: PolyBrik draws to fb0 and pans it; the menu's pan offset is restored on exit and
#   fb0 is not blanked (POLYBRIK_KEEP_FB), so the menu shows again.
# - A first-run report in the log (display, input and audio as Knulli sets them up).

APP_DIR="$(cd "$(dirname "$0")/PolyBrik" && pwd)"
export SDCARD_PATH="${SDCARD_PATH:-/userdata}"
export LOGS_PATH="${LOGS_PATH:-$APP_DIR}"
export POLYBRIK_KEEP_FB=1
SD="$SDCARD_PATH/PolyBrik"
mkdir -p "$SD"
LOG="$LOGS_PATH/PolyBrik.txt"
AUDIO_INIT=/etc/init.d/S06audio
FB_PAN=/sys/class/graphics/fb0/pan

if [ -z "$DEVICE" ]; then
    if [ -s "$SD/device" ]; then
        DEVICE="$(head -1 "$SD/device" | tr -d ' \r')"
    elif grep -q "TRIMUI Brick Pro Controller" /sys/class/input/event*/device/name 2>/dev/null; then
        DEVICE=brickpro-knulli
    else
        DEVICE=brickpro
    fi
fi
export DEVICE

PAN_SAVED="$(cat "$FB_PAN" 2>/dev/null)"
{
    echo "=== $(date) knulli: DEVICE=$DEVICE (create $SD/device to override), fb0 pan $PAN_SAVED"
    uname -a
    echo "-- fb0: $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null) bpp $(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)"
    for e in /sys/class/input/event*; do echo "-- $(basename "$e"): $(cat "$e/device/name" 2>/dev/null)"; done
} >>"$LOG" 2>&1

# ---- Audio service off while PolyBrik runs -----------------------------------------------------
PW_STOPPED=0
if pidof pipewire >/dev/null 2>&1; then
    echo "knulli: stopping the audio service for direct ALSA access" >>"$LOG"
    if [ -x "$AUDIO_INIT" ]; then
        "$AUDIO_INIT" stop >>"$LOG" 2>&1
    else
        killall wireplumber pipewire 2>/dev/null
    fi
    for i in 1 2 3 4 5 6 7 8 9 10; do
        pidof pipewire >/dev/null 2>&1 || break
        sleep 0.2
    done
    PW_STOPPED=1
fi

sh "$APP_DIR/launch.sh"
rc=$?

if [ "$PW_STOPPED" = 1 ]; then
    echo "knulli: starting the audio service again" >>"$LOG"
    if [ -x "$AUDIO_INIT" ]; then
        # restart, not start: start also runs `alsactl init`, which resets the mixer levels.
        "$AUDIO_INIT" restart >>"$LOG" 2>&1
    fi
fi
[ -n "$PAN_SAVED" ] && echo "$PAN_SAVED" >"$FB_PAN" 2>/dev/null
echo "knulli: exit $rc, fb0 pan now $(cat "$FB_PAN" 2>/dev/null)" >>"$LOG"
exit $rc
