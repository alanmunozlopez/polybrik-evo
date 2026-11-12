#!/bin/sh
# HELP: PolyBrik: the Polyend Tracker Mini firmware, with USB MIDI and USB audio
# ICON: polybrik
# GRID: PolyBrik
#
# MustardOS (muOS) launcher for PolyBrik on the TrimUI Brick / Brick Pro. EXPERIMENTAL: written
# from muOS documentation, not yet run on a device; check $APP_DIR/PolyBrik.txt after a first run.
#
# Install: copy the folder that holds this file, tracker-sdl, content/ and content.version (the
# same files as PolyBrik.pak, plus this script and launch.sh) to /mnt/mmc/MUOS/application/PolyBrik/.
# PolyBrik keeps its projects in /mnt/mmc/PolyBrik, as it keeps them in /mnt/SDCARD/PolyBrik on NextUI.
#
# What this adds to launch.sh (the NextUI launcher, shared):
# - SDCARD_PATH and LOGS_PATH for muOS.
# - DEVICE (NextUI sets it, muOS does not): from $SD/device if you create it (one word: brick or
#   brickpro), else guessed from the muOS board name.
# - The codec: muOS runs PipeWire, which keeps the Brick's sound card (hw:0,0) open. PolyBrik is a
#   static binary and talks to ALSA directly, so PipeWire is stopped while PolyBrik runs and started
#   again, with the same command line and environment, when PolyBrik exits.

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
export SDCARD_PATH="${SDCARD_PATH:-/mnt/mmc}"
export LOGS_PATH="${LOGS_PATH:-$APP_DIR}"
SD="$SDCARD_PATH/PolyBrik"
mkdir -p "$SD"
LOG="$LOGS_PATH/PolyBrik.txt"

if [ -z "$DEVICE" ]; then
    if [ -s "$SD/device" ]; then
        DEVICE="$(head -1 "$SD/device" | tr -d ' \r')"
    else
        BOARD="$(cat /opt/muos/device/config/board/name 2>/dev/null || cat /opt/muos/config/device.txt 2>/dev/null)"
        case "$BOARD" in
            *[Pp][Rr][Oo]*) DEVICE=brickpro ;;
            *) DEVICE=brick ;;
        esac
        echo "muos: board '$BOARD' -> DEVICE=$DEVICE (create $SD/device to override)" >>"$LOG"
    fi
fi
export DEVICE

# ---- PipeWire off while PolyBrik runs --------------------------------------------------------
PW_SAVE="/tmp/polybrik-pipewire.$$"
: >"$PW_SAVE"
for pid in $(pidof pipewire wireplumber pipewire-pulse 2>/dev/null); do
    [ -r "/proc/$pid/cmdline" ] || continue
    # One line per process: XDG_RUNTIME_DIR, then the NUL-separated command line as words.
    xdg="$(tr '\0' '\n' <"/proc/$pid/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p')"
    cmd="$(tr '\0' ' ' <"/proc/$pid/cmdline")"
    echo "$xdg|$cmd" >>"$PW_SAVE"
done
if [ -s "$PW_SAVE" ]; then
    echo "muos: stopping PipeWire for direct ALSA access:" >>"$LOG"
    cat "$PW_SAVE" >>"$LOG"
    killall wireplumber pipewire-pulse pipewire 2>/dev/null
    for i in 1 2 3 4 5 6 7 8 9 10; do
        pidof pipewire >/dev/null 2>&1 || break
        sleep 0.2
    done
fi
restore_pipewire() {
    [ -s "$PW_SAVE" ] || { rm -f "$PW_SAVE"; return; }
    # pipewire first, then its session manager and the pulse shim, as they were started.
    for pass in pipewire wireplumber pipewire-pulse; do
        while IFS='|' read -r xdg cmd; do
            set -- $cmd
            [ "$(basename "$1")" = "$pass" ] || continue
            echo "muos: restarting $cmd" >>"$LOG"
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
