#!/bin/sh
# Screen regression suite on the device: runs every case of screens.txt headless on the Brick in one SSH
# session, brings the screenshots back and compares them with golden/<name>.png pixel by pixel.
#   platforms/sdl/devtests/brick-suite.sh [--update] [--os knulli|nextui] [--only NAME] [--cases FILE] [HOST]
# HOST: an SSH host or alias (default $BRICK_HOST, else knulli / brick). BRICK_SSH_OPTS is passed to
# ssh and scp (for example "-F path/to/ssh_config"). BIN=path/to/tracker-sdl runs that local binary.
# --update writes the screenshots as the new goldens instead of comparing.
# Never touches the screen or the user's SD folder: the app runs from /tmp with a fresh SD made from
# the pak's factory content. The installed app must not be running (checked).
# Results: build-suite/<os>/<name>.png, and <name>.diff.png for every case that differs.
set -eu
UPDATE=0; OS=knulli; ONLY=""; CASES_FILE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --update) UPDATE=1 ;;
        --os) OS="$2"; shift ;;
        --only) ONLY="$2"; shift ;;
        --cases) CASES_FILE="$2"; shift ;;
        *) break ;;
    esac
    shift
done
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
case "$OS" in
    knulli) APP=/userdata/roms/ports/PolyBrik; DEV=brickpro-knulli; LIBS=""; DEFHOST=knulli ;;
    nextui) APP=/mnt/SDCARD/Tools/tg5040/PolyBrik.pak; DEV=brickpro; LIBS="/usr/trimui/lib:/usr/lib"; DEFHOST=brick ;;
    *) echo "--os knulli or nextui"; exit 2 ;;
esac
HOST="${1:-${BRICK_HOST:-$DEFHOST}}"
SSH_OPTS="${BRICK_SSH_OPTS:-}"
OUT="$ROOT/build-suite/$OS"
GOLD="$HERE/golden"
rm -rf "$OUT"; mkdir -p "$OUT" "$GOLD"

# Cases as "name at keys" lines; keys "-" means none.
CASES="$(grep -v -E '^[[:space:]]*(#|$)' "${CASES_FILE:-$HERE/screens.txt}" | awk -v only="$ONLY" 'only == "" || $1 == only { print $1, $2, $3, ($4 == "" ? "-" : $4) }')"
[ -n "$CASES" ] || { echo "no case matches"; exit 2; }

BIN_REMOTE="$APP/tracker-sdl"
if [ -n "${BIN:-}" ]; then
    ssh $SSH_OPTS "$HOST" "mkdir -p /tmp/pbsuite-bin"
    STRIP="${STRIP:-arm-gcc10-linux-gnueabihf-strip}"
    "$STRIP" -o "$OUT/.tracker-sdl" "$BIN" 2>/dev/null || cp "$BIN" "$OUT/.tracker-sdl"
    scp -q $SSH_OPTS "$OUT/.tracker-sdl" "$HOST:/tmp/pbsuite-bin/tracker-sdl"
    rm -f "$OUT/.tracker-sdl"
    BIN_REMOTE=/tmp/pbsuite-bin/tracker-sdl
fi

# One remote shell runs every case. The device has no `timeout`; --quit-after ends each run.
printf '%s\n' "$CASES" | ssh $SSH_OPTS "$HOST" "cat >/tmp/pbsuite-cases"
ssh $SSH_OPTS "$HOST" "
pidof tracker-sdl >/dev/null 2>&1 && { echo 'PolyBrik is running on the device, quit it first'; exit 1; }
rm -rf /tmp/pbsuite; mkdir -p /tmp/pbsuite/out
cp '$BIN_REMOTE' /tmp/pbsuite/tracker-sdl
cd /tmp/pbsuite
while read -r name at keys check; do
    rm -rf sd; cp -R '$APP/content' sd
    set -- --sd /tmp/pbsuite/sd --no-window --from-ready --device $DEV \
        --screenshot out/\$name.png --screenshot-at \$at --quit-after \$((\${at##*,} + 200))
    case \"\$check\" in
        audio:*) set -- \"\$@\" --audio-file out/\$name.wav ;;   # real time: the WAV follows the wall clock
        *) set -- \"\$@\" --no-audio --sim-clock ;;
    esac
    [ \"\$keys\" = - ] || set -- \"\$@\" --keys \"\$keys\"
    LD_LIBRARY_PATH='$LIBS' ./tracker-sdl \"\$@\" >out/\$name.log 2>&1
    rc=\$?
    ls out | grep -q \"^\$name[.-]\" || echo \"FAILED \$name: exit \$rc, no screenshot\"
done </tmp/pbsuite-cases
rm -f /tmp/pbsuite-cases
" || exit 1
ssh $SSH_OPTS "$HOST" "cd /tmp/pbsuite/out && tar cf - . && rm -rf /tmp/pbsuite /tmp/pbsuite-bin" | tar xf - -C "$OUT"

# Compare (or update the goldens).
fail=0
# A case with several screenshot times gives <name>-1.png, <name>-2.png, ...; each is compared.
for name in $(printf '%s\n' "$CASES" | awk '{print $1}'); do
    check="$(printf '%s\n' "$CASES" | awk -v n="$name" '$1 == n { print $4 }')"
    case "$check" in
        audio:*)
            # Audio cases run in real time: their screenshots vary, the WAV is what is checked.
            if res="$(python3 -I "$HERE/wavcheck.py" "$OUT/$name.wav" "${check#audio:}")"; then
                echo "ok    $name: $res"
            else
                echo "FAIL  $name (want ${check#audio:}): $res"; fail=1
            fi
            continue ;;
    esac
    shots="$(ls "$OUT/$name.png" "$OUT/$name"-[0-9]*.png 2>/dev/null | grep -v '\.diff\.png$' || true)"
    want="$(printf '%s\n' "$CASES" | awk -v n="$name" '$1 == n { print split($2, t, ",") }')"
    got="$(printf '%s\n' "$shots" | grep -c . || true)"
    if [ "$got" != "$want" ]; then
        echo "FAIL  $name: $got of $want screenshots ($(grep -i -E 'fatal|signal|error' "$OUT/$name.log" 2>/dev/null | head -1))"; fail=1; continue
    fi
    for shot in $shots; do
    case_id="$(basename "$shot" .png)"
    if [ "$UPDATE" = 1 ]; then
        cp "$shot" "$GOLD/$case_id.png"; echo "gold  $case_id"
    elif [ ! -s "$GOLD/$case_id.png" ]; then
        echo "NEW   $case_id: no golden yet (run with --update)"; fail=1
    elif res="$(python3 -I "$HERE/pngdiff.py" "$GOLD/$case_id.png" "$shot" "$OUT/$case_id.diff.png")"; then
        echo "ok    $case_id"
    else
        echo "DIFF  $case_id: $res"; fail=1
    fi
    done
done
echo "screenshots in $OUT"
exit $fail
