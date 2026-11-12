#!/bin/sh
# PolyBrik USB MIDI / USB audio diagnostic for the TrimUI Brick and Brick Pro (NextUI, MustardOS,
# Knulli, stock). Read-only: it changes nothing on the device, it only reports what the kernel and the
# OS offer. Plug your USB MIDI controller (and a USB audio interface if you have one) into the host
# USB-C port first, ideally through a powered hub.
#
# Run it over SSH (sh midi-diag.sh), or as a NextUI pak / muOS application (see devtests/README.md).
# The report goes to stdout and to polybrik-midi-diag.txt at the SD card root.

for r in "$SDCARD_PATH" /mnt/SDCARD /mnt/sdcard /mnt/mmc /userdata /tmp; do
    [ -n "$r" ] && [ -d "$r" ] && [ -w "$r" ] && { ROOT="$r"; break; }
done
OUT="$ROOT/polybrik-midi-diag.txt"

section() { printf '\n==== %s\n' "$1"; }
show() { [ -e "$1" ] && { echo "-- $1"; cat "$1" 2>&1; }; }

{
echo "PolyBrik MIDI diagnostic, $(date)"

section "system"
uname -a
show /etc/os-release
for f in /etc/version /usr/trimui/version /opt/muos/config/system/version /opt/muos/config/version.txt; do show "$f"; done
echo "DEVICE=$DEVICE PLATFORM=$PLATFORM"
[ -d /opt/muos ] && echo "MustardOS (muOS) detected"
[ -d /mnt/SDCARD/.system ] && echo "NextUI/MinUI layout detected"
[ -e /usr/share/batocera ] || [ -d /userdata/system ] && echo "Knulli/Batocera layout detected"
show /proc/device-tree/model; echo
cat /proc/cpuinfo 2>/dev/null | grep -i -E "^hardware|^model name" | sort -u

section "ALSA: sound cards (a USB device shows as USB-Audio)"
show /proc/asound/cards
show /proc/asound/devices
echo "-- /dev/snd"; ls -l /dev/snd 2>&1
echo "-- raw MIDI ports (what PolyBrik opens)"
found=0
for m in /dev/snd/midiC*D*; do
    [ -e "$m" ] || continue
    found=1
    c="${m#/dev/snd/midiC}"; c="${c%%D*}"; d="${m##*D}"
    echo "$m  card $c: $(cat /proc/asound/card$c/id 2>/dev/null)  $(head -1 /proc/asound/card$c/midi$d 2>/dev/null)"
done
[ "$found" = 1 ] || echo "NONE (no /dev/snd/midiC*D*)"
for d in /proc/asound/card[0-9]*; do
    [ -e "$d/usbid" ] || continue
    echo "-- USB card $d: usbid $(cat $d/usbid)"
    ls "$d"
    for s in "$d"/stream*; do show "$s"; done
done

section "kernel modules"
echo "-- loaded (snd / usb)"
(lsmod 2>/dev/null || cat /proc/modules) | grep -i -E "snd|usb|midi|uac|gadget|libcomposite" || echo "(none listed)"
echo "-- available .ko for snd-usb-audio / rawmidi / gadgets"
find /lib/modules /vendor/modules /system/lib/modules -name "*.ko" 2>/dev/null \
    | grep -i -E "snd[-_]usb|usbmidi|rawmidi|snd[-_]hwdep|f_midi|u_audio|uac|libcomposite|usb_f_" || echo "(none found)"
echo "-- kernel config"
if [ -e /proc/config.gz ]; then
    zcat /proc/config.gz | grep -E "CONFIG_SND_USB_AUDIO|CONFIG_SND_RAWMIDI|CONFIG_SND_SEQUENCER|CONFIG_SND_USB|CONFIG_USB_CONFIGFS|CONFIG_USB_F_MIDI|CONFIG_USB_F_UAC|CONFIG_USB_F_FS|CONFIG_USB_GADGET|CONFIG_USB_MUSB|CONFIG_USB_SUNXI|CONFIG_USB_OTG|CONFIG_HIGH_RES_TIMERS|CONFIG_PREEMPT"
else
    echo "(no /proc/config.gz)"
fi
echo "-- built-in drivers"
ls /sys/bus/usb/drivers 2>&1 | tr '\n' ' '; echo
[ -d /sys/module/snd_usb_audio ] && echo "snd_usb_audio: present in /sys/module"
[ -d /sys/module/snd_rawmidi ] && echo "snd_rawmidi: present in /sys/module"

section "USB devices"
command -v lsusb >/dev/null && lsusb
for u in /sys/bus/usb/devices/*; do
    [ -e "$u/idVendor" ] || continue
    echo "$(basename $u): $(cat $u/idVendor):$(cat $u/idProduct) $(cat $u/manufacturer 2>/dev/null) $(cat $u/product 2>/dev/null) speed=$(cat $u/speed 2>/dev/null) maxpower=$(cat $u/bMaxPower 2>/dev/null)"
    for i in "$u"/*:*; do
        [ -e "$i/bInterfaceClass" ] && echo "    $(basename $i) class=$(cat $i/bInterfaceClass) sub=$(cat $i/bInterfaceSubClass) driver=$(basename "$(readlink $i/driver 2>/dev/null)" 2>/dev/null)"
    done
done

section "USB role / OTG (which port is host)"
for f in /sys/devices/platform/soc/usbc0/otg_role /sys/devices/platform/soc/usbc0/usb_host \
         /sys/devices/platform/soc*/*usbc*/otg_role /sys/class/usb_role/*/role /sys/devices/platform/*/otg_role; do
    [ -e "$f" ] && echo "$f: $(cat $f 2>&1)"
done
ls /sys/class/udc 2>/dev/null | sed 's/^/udc: /'
[ -d /sys/kernel/config/usb_gadget ] && { echo "configfs gadgets:"; ls /sys/kernel/config/usb_gadget; }
grep -i -E "usb|otg" /proc/interrupts 2>/dev/null

section "audio services (who holds the codec)"
ps 2>/dev/null | grep -i -E "pipewire|wireplumber|pulse|alsa|audioserver" | grep -v grep || echo "(none)"
for p in /proc/asound/card*/pcm*p/sub*/status; do
    [ -e "$p" ] && echo "$p: $(head -1 $p)"
done
show /etc/asound.conf
ls /usr/share/alsa/alsa.conf 2>&1

section "framebuffer / input"
cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel 2>&1
ls /dev/input 2>&1 | tr '\n' ' '; echo
for e in /sys/class/input/event*; do echo "$(basename $e): $(cat $e/device/name 2>/dev/null)"; done

section "live watch (${WATCH_SECS:=60} s): plug, unplug and replug the device now"
# One line every 2 s with the USB devices and raw MIDI ports present, so a device that enumerates
# late or drops after a few seconds shows up. The menu comes back when it ends.
t=0
while [ "$t" -lt "$WATCH_SECS" ]; do
    devs=""
    for u in /sys/bus/usb/devices/*; do
        [ -e "$u/idVendor" ] || continue
        [ "$(cat $u/idVendor)" = "1d6b" ] && continue    # root hubs
        devs="$devs $(basename $u)=$(cat $u/idVendor):$(cat $u/idProduct)($(cat $u/product 2>/dev/null))"
    done
    printf '%3ss usb:%s | midi: %s\n' "$t" "${devs:- none}" "$(ls /dev/snd 2>/dev/null | grep midi | tr '\n' ' ')"
    sleep 2
    t=$((t + 2))
done

section "kernel log (usb / snd)"
dmesg 2>/dev/null | grep -i -E "usb|snd|midi|audio|otg|vbus|axp" | tail -150
dmesg >"$ROOT/polybrik-dmesg.txt" 2>&1
echo "(full kernel log in $ROOT/polybrik-dmesg.txt)"

section "verdict"
if ls /dev/snd/midiC*D* >/dev/null 2>&1; then
    echo "OK: raw MIDI ports present. PolyBrik's USB MIDI will work on this OS."
elif grep -q USB-Audio /proc/asound/cards 2>/dev/null; then
    echo "PARTIAL: a USB audio card is present but no raw MIDI port. The device may not be MIDI, or snd-rawmidi is missing."
elif [ -d /sys/module/snd_usb_audio ]; then
    echo "DRIVER OK, NO DEVICE: snd-usb-audio is in the kernel but nothing enumerated. Check the port (host is the top one on the Brick), the cable (data, not charge-only) and power (use a powered hub)."
else
    echo "NO DRIVER: snd-usb-audio is not in this kernel (or nothing is plugged in). This OS cannot do USB MIDI without a kernel with snd-usb-audio."
fi
} 2>&1 | tee "$OUT"
echo
echo "report written to $OUT"
