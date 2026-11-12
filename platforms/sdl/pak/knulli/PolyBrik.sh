#!/bin/bash
# Knulli (Batocera-based) launcher for PolyBrik on the TrimUI Brick / Brick Pro. EXPERIMENTAL: written
# from Knulli's ports conventions, not yet run on a device; read PolyBrik/PolyBrik.txt after a first run.
#
# Install: copy this file and the PolyBrik folder next to it (tracker-sdl, launch.sh, content/,
# content.version) to /userdata/roms/ports/, then update the game list in the Ports system.
# PolyBrik keeps its projects in /userdata/PolyBrik, as it keeps them in /mnt/SDCARD/PolyBrik on NextUI.
#
# What this adds to launch.sh (the NextUI launcher, shared):
# - SDCARD_PATH and LOGS_PATH for Knulli.
# - DEVICE: from /userdata/PolyBrik/device if you create it (one word: brick or brickpro), else brickpro.
# - The codec: Knulli runs PipeWire, which can keep the Brick's sound card (hw:0,0) open. PolyBrik is a
#   static binary and talks to ALSA directly, so PipeWire is stopped while PolyBrik runs and started
#   again, with the same command line and environment, when PolyBrik exits.
# - A first-run report in the log (display, input and audio as Knulli sets them up), to fix what differs.

APP_DIR="$(cd "$(dirname "$0")/PolyBrik" && pwd)"
export SDCARD_PATH="${SDCARD_PATH:-/userdata}"
export LOGS_PATH="${LOGS_PATH:-$APP_DIR}"
SD="$SDCARD_PATH/PolyBrik"
mkdir -p "$SD"
LOG="$LOGS_PATH/PolyBrik.txt"

if [ -z "$DEVICE" ]; then
    if [ -s "$SD/device" ]; then
        DEVICE="$(head -1 "$SD/device" | tr -d ' \r')"
    else
        DEVICE=brickpro
    fi
fi
export DEVICE

{
    echo "=== $(date) knulli: DEVICE=$DEVICE (create $SD/device to override)"
    uname -a
    cat /usr/share/batocera/batocera.version 2>/dev/null
    echo "-- fb0: $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null) bpp $(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)"
    ls /dev/dri 2>/dev/null | sed 's/^/-- dri: /'
    for e in /sys/class/input/event*; do echo "-- $(basename "$e"): $(cat "$e/device/name" 2>/dev/null)"; done
    ps 2>/dev/null | grep -i -E "pipewire|wireplumber|pulse|emulationstation" | grep -v grep
} >>"$LOG" 2>&1

# ---- PipeWire off while PolyBrik runs --------------------------------------------------------
PW_SAVE="/tmp/polybrik-pipewire.$$"
: >"$PW_SAVE"
for pid in $(pidof pipewire wireplumber pipewire-pulse 2>/dev/null); do
    [ -r "/proc/$pid/cmdline" ] || continue
    xdg="$(tr '\0' '\n' <"/proc/$pid/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p')"
    cmd="$(tr '\0' ' ' <"/proc/$pid/cmdline")"
    echo "$xdg|$cmd" >>"$PW_SAVE"
done
if [ -s "$PW_SAVE" ]; then
    echo "knulli: stopping PipeWire for direct ALSA access:" >>"$LOG"
    cat "$PW_SAVE" >>"$LOG"
    killall wireplumber pipewire-pulse pipewire 2>/dev/null
    for i in 1 2 3 4 5 6 7 8 9 10; do
        pidof pipewire >/dev/null 2>&1 || break
        sleep 0.2
    done
fi
restore_pipewire() {
    [ -s "$PW_SAVE" ] || { rm -f "$PW_SAVE"; return; }
    for pass in pipewire wireplumber pipewire-pulse; do
        while IFS='|' read -r xdg cmd; do
            set -- $cmd
            [ "$(basename "$1")" = "$pass" ] || continue
            echo "knulli: restarting $cmd" >>"$LOG"
            XDG_RUNTIME_DIR="${xdg:-/run}" nohup $cmd >/dev/null 2>&1 &
            sleep 0.3
        done <"$PW_SAVE"
    done
    rm -f "$PW_SAVE"
}

sh "$APP_DIR/launch.sh"
rc=$?
restore_pipewire
exit $rc
