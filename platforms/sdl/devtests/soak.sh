#!/bin/sh
# Stability (soak) test on the device, in two runs of the app:
#   1. build: --sim-clock, no audio. Keys make a two-pattern song and save it as test-project. The
#      simulated clock makes this run the same every time (in real time, Play toggles could land
#      differently and leave the song stopped).
#   2. play: real time. The app opens test-project again (the last project), Song Play starts the song,
#      and it plays for N minutes through the Brick's own audio output. The process is sampled every
#      10 s and the run gets a verdict.
#   platforms/sdl/devtests/soak.sh [--os knulli|nextui] [--minutes N] [--wav] [HOST]
# HOST, BRICK_SSH_OPTS and BIN as in brick-suite.sh. --wav renders to a WAV in real time instead of the
# codec (no audio service is touched; the WAV is checked with wavcheck.py).
#
# The project, made in a fresh SD from the pak's factory content (nothing of the user's is touched):
#   Pattern 1: 808_BD2 (instrument 1) on every beat on track 1, Synth 2 (ACD) eighths on track 2.
#   Pattern 2: ACD eighths on track 1, Synth 1 on every beat on track 2.
#   Song: 1, 2, 1, 2, played with Song Play from the first slot; Song mode loops at the end.
# Knulli: the audio service (PipeWire) is stopped for the run and started again with Knulli's own init
# script, as the Knulli launcher does; the menu loses its sound for the length of the run.
# The run is detached on the device, so a dropped SSH connection does not stop it.
#
# Pass: no underrun after the first 30 s, the worst block render under half of a period, memory
# (VmRSS) growth under 1 MB from the first minute to the end, and the app still running at the end.
# Results in build-suite/soak-<os>/: build.log, build-1..2.png (patterns, saved song), soak.log (app
# stderr of the play run), samples.tsv, shot-1..3.png (song started, middle, end), report.txt.
set -eu
OS=knulli; MINUTES=6; WAV=0
while [ $# -gt 0 ]; do
    case "$1" in
        --os) OS="$2"; shift ;;
        --minutes) MINUTES="$2"; shift ;;
        --wav) WAV=1 ;;
        *) break ;;
    esac
    shift
done
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
case "$OS" in
    knulli) APP=/userdata/roms/ports/PolyBrik; DEV=brickpro-knulli; LIBS=""; DEFHOST=knulli; AUDIO_INIT=/etc/init.d/S06audio ;;
    nextui) APP=/mnt/SDCARD/Tools/tg5040/PolyBrik.pak; DEV=brickpro; LIBS="/usr/trimui/lib:/usr/lib"; DEFHOST=brick; AUDIO_INIT="" ;;
    *) echo "--os knulli or nextui"; exit 2 ;;
esac
[ "$WAV" = 1 ] && AUDIO_INIT=""
HOST="${1:-${BRICK_HOST:-$DEFHOST}}"
SSH_OPTS="${BRICK_SSH_OPTS:-}"
OUT="$ROOT/build-suite/soak-$OS"
rm -rf "$OUT"; mkdir -p "$OUT"

# ---- Run 1: the keys that build the song and save it (simulated ms from the first settled page) ----
k="300:sample_load,450:down,600:down,750:enter,900:down,1050:enter,1200:down,1350:down,1500:down,1650:enter,1800:down,1950:enter,2100:down,2300:screen1,2600:pattern"
seq_keys() { # key start_ms step_ms count
    i=0; t=$2
    while [ $i -lt $4 ]; do k="$k,${t}:$1"; t=$((t + $3)); i=$((i + 1)); done
}
k="$k,2800:screen3:700,3000:plus1,3200:plus1,3400:plus1,3700:rec"                   # step jump 4, edit mode
seq_keys enter 3900 200 8                                                           # P1 t1: kick on every beat
k="$k,5800:right,5950:right,6100:right,6250:right,6500:enter,6700:up,6800:up,6900:up,7000:up"
k="$k,7200:instr,7400:minus1,7600:minus1,7900:note,8100:screen3:600,8300:minus1,8500:minus1"   # Synth 2, jump 2
seq_keys enter 8900 200 16                                                          # P1 t2: ACD eighths
k="$k,12400:screen1:500,12600:plus1,12900:left,13050:left,13200:left,13350:left"    # pattern 2, track 1
seq_keys enter 13600 200 16                                                         # P2 t1: ACD eighths
k="$k,17000:right,17150:right,17300:right,17450:right,17700:enter,17900:up,18000:up"
k="$k,18200:instr,18400:minus1,18700:note,18900:screen3:600,19100:plus1,19300:plus1"         # Synth 1, jump 4
seq_keys enter 19700 200 8                                                          # P2 t2: Synth 1 on every beat
k="$k,21600:rec,21800:play"                                                         # edit mode off, stop
# Song: rec, Add Slot / pattern -+ to 1 2 1 2, rec off, back to the first slot, Song Play
k="$k,22100:song,22300:rec,22600:screen4,22900:screen7,23200:screen4,23500:screen6,23800:screen4,24100:screen7,24400:rec"
k="$k,24600:up,24800:up,25000:up,25400:screen2,26000:play"                       # play it, stop
# Save As test-project: File, Save As (screen5), the name typed on the on-screen keyboard, Save (screen8);
# the same keys as project_cycle in screens.txt.
k="$k,26700:file,27000:screen5,27300:screen8,27600:down,27750:right,27900:right,28050:right,28200:right"
k="$k,28350:down,28500:screen1,28650:left,28800:left,28950:screen1,29100:left,29250:down,29400:screen1"
k="$k,29550:right,29700:right,29850:right,30000:up,30150:screen1,30300:left,30450:left,30600:left"
k="$k,30750:left,30900:left,31050:left,31200:screen1,31350:left,31500:screen1,31650:left,31800:left"
k="$k,31950:left,32100:left,32250:left,32400:left,32550:screen1,32700:right,32850:right,33000:right"
k="$k,33150:right,33300:right,33450:screen1,33600:left,33750:left,33900:down,34050:screen1,34200:left"
k="$k,34350:left,34500:left,34650:left,34800:up,34950:screen1,35100:down,35250:down,35400:screen1"
k="$k,35550:right,35700:right,35850:up,36000:up,36150:screen1,36500:screen8"
BUILD_KEYS="$k"
BUILD_MS=37500
# ---- Run 2: Song, Song Play; then nothing but playback (real ms) -------------------------------------
PLAY_KEYS="300:song,600:screen2"
RUN_MS=$((1000 + MINUTES * 60000))
SHOTS="1500,$((MINUTES * 30000)),$((RUN_MS - 2000))"

BIN_REMOTE="$APP/tracker-sdl"
if [ -n "${BIN:-}" ]; then
    STRIP="${STRIP:-arm-gcc10-linux-gnueabihf-strip}"
    "$STRIP" -o "$OUT/.tracker-sdl" "$BIN" 2>/dev/null || cp "$BIN" "$OUT/.tracker-sdl"
    ssh $SSH_OPTS "$HOST" "mkdir -p /tmp/pbsoak-bin"
    scp -q $SSH_OPTS "$OUT/.tracker-sdl" "$HOST:/tmp/pbsoak-bin/tracker-sdl"
    rm -f "$OUT/.tracker-sdl"
    BIN_REMOTE=/tmp/pbsoak-bin/tracker-sdl
fi
if [ "$WAV" = 1 ]; then AUDIO_ARGS="--audio-file out/soak.wav"; else AUDIO_ARGS=""; fi

# ---- The run, detached on the device ----------------------------------------------------------------
cat >"$OUT/.run.sh" <<EOF
#!/bin/sh
cd /tmp/pbsoak
restore() { [ -n "$AUDIO_INIT" ] && "$AUDIO_INIT" restart >out/audio-service.txt 2>&1; touch out/done; }
trap restore EXIT
[ -n "$AUDIO_INIT" ] && pidof pipewire >/dev/null 2>&1 && "$AUDIO_INIT" stop >out/audio-service.txt 2>&1
LD_LIBRARY_PATH='$LIBS' ./tracker-sdl --sd /tmp/pbsoak/sd --no-window --from-ready --sim-clock --no-audio \\
    --device $DEV --keys '$BUILD_KEYS' --screenshot out/build.png --screenshot-at 21400,37200 \\
    --quit-after $BUILD_MS >out/build.log 2>&1
AUDIODEV=hw:0,0 LD_LIBRARY_PATH='$LIBS' ./tracker-sdl --sd /tmp/pbsoak/sd --no-window --from-ready \\
    --device $DEV $AUDIO_ARGS --keys '$PLAY_KEYS' --screenshot out/shot.png --screenshot-at $SHOTS \\
    --quit-after $RUN_MS >out/soak.log 2>&1 &
pid=\$!
printf 's\trss_kb\tcpu_pct\ttemp_mc\tfreq_khz\tunderruns\n' >out/samples.tsv
s=0; last=0
while kill -0 \$pid 2>/dev/null; do
    ticks=\$(awk '{print \$14 + \$15}' /proc/\$pid/stat 2>/dev/null || echo 0)
    rss=\$(awk '/VmRSS/ {print \$2}' /proc/\$pid/status 2>/dev/null || echo 0)
    temp=\$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)
    freq=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)
    under=\$(grep -c underrun out/soak.log 2>/dev/null || true)
    [ \$s -gt 0 ] && printf '%s\t%s\t%s\t%s\t%s\t%s\n' \$s "\$rss" \$(( (ticks - last) * 100 / 1000 )) "\$temp" "\$freq" "\$under" >>out/samples.tsv
    last=\$ticks
    sleep 10
    s=\$((s + 10))
done
wait \$pid
echo \$? >out/exit-code
EOF
ssh $SSH_OPTS "$HOST" "pidof tracker-sdl >/dev/null 2>&1 && { echo 'PolyBrik is running on the device, quit it first'; exit 1; }
rm -rf /tmp/pbsoak; mkdir -p /tmp/pbsoak/out; cp -R '$APP/content' /tmp/pbsoak/sd; cp '$BIN_REMOTE' /tmp/pbsoak/tracker-sdl
cat >/tmp/pbsoak/run.sh" <"$OUT/.run.sh"
rm -f "$OUT/.run.sh"
ssh $SSH_OPTS "$HOST" "nohup sh /tmp/pbsoak/run.sh >/dev/null 2>&1 &"
echo "soak: build the song, then $MINUTES min of Song playback ($( [ "$WAV" = 1 ] && echo 'WAV' || echo 'device audio'))"

# ---- Wait, then fetch ------------------------------------------------------------------------------
waited=0
while :; do
    sleep 30
    waited=$((waited + 30))
    if ssh $SSH_OPTS "$HOST" "test -e /tmp/pbsoak/out/done" 2>/dev/null; then break; fi
    echo "  ${waited} s: $(ssh $SSH_OPTS "$HOST" "tail -1 /tmp/pbsoak/out/samples.tsv" 2>/dev/null | awk -F'\t' '{printf "rss %s kB, cpu %s%%, %d C, %s underruns", $2, $3, $4 / 1000, $6}')"
    [ $waited -gt $((MINUTES * 60 + 300)) ] && { echo "soak: no end after $waited s"; break; }
done
ssh $SSH_OPTS "$HOST" "cd /tmp/pbsoak/out && tar cf - ." | tar xf - -C "$OUT"
ssh $SSH_OPTS "$HOST" "rm -rf /tmp/pbsoak /tmp/pbsoak-bin"

# ---- Verdict ---------------------------------------------------------------------------------------
period_us=$(sed -n 's/.*obtained [0-9]* Hz fmt [0-9a-fx]* [0-9]* ch \([0-9]*\) frames.*/\1/p' "$OUT/soak.log" | head -1)
period_us=$(( ${period_us:-512} * 1000000 / 44100 ))
max_render=$(sed -n 's/.*max render \([0-9]*\) us.*/\1/p' "$OUT/soak.log" | tail -1)
late=$(sed -n 's/.*callbacks, \([0-9]*\) late callbacks.*/\1/p' "$OUT/soak.log" | tail -1)
exit_code=$(cat "$OUT/exit-code" 2>/dev/null || echo none)
crash=$(grep -i -E 'fatal|signal' "$OUT/soak.log" | head -1 || true)
awk -F'\t' -v warm=30 -v first=60 -v maxr="${max_render:-0}" -v period="$period_us" -v late="${late:-?}" \
    -v ec="$exit_code" -v crash="$crash" -v wav="$WAV" '
NR == 1 { next }
{ n++; if ($1 <= warm) base = $6; if (rss0 == "" && $1 >= first) rss0 = $2; rss1 = $2
  if ($4 > tmax) tmax = $4; if (fmin == "" || $5 < fmin) fmin = $5; if ($5 > fmax) fmax = $5
  cpu += $3; if ($3 > cpumax) cpumax = $3; under = $6 }
END {
  after = under - base; grow = rss1 - rss0
  printf "samples %d (%d s), exit %s%s\n", n, n * 10, ec, (crash != "" ? ", " crash : "")
  printf "underruns %d (%d after the first %d s), late callbacks %s\n", under, after, warm, late
  printf "worst render %d us of a %d us period (%.0f%%)\n", maxr, period, 100 * maxr / period
  printf "memory %d kB at %d s, %d kB at the end (%+d kB)\n", rss0, first, rss1, grow
  printf "cpu %.0f%% average, %d%% peak; %.1f C peak; cpu0 %d..%d MHz\n", cpu / n, cpumax, tmax / 1000, fmin / 1000, fmax / 1000
  ok = (after == 0 || wav) && maxr * 2 < period && grow < 1024 && ec == "0" && crash == ""
  print (ok ? "PASS" : "FAIL")
  exit !ok
}' "$OUT/samples.tsv" | tee "$OUT/report.txt"
verdict=$?
if [ "$WAV" = 1 ] && [ -s "$OUT/soak.wav" ]; then
    python3 -I "$HERE/wavcheck.py" "$OUT/soak.wav" sound | sed 's/^/wav: /' | tee -a "$OUT/report.txt"
fi
echo "results in $OUT"
exit $verdict
