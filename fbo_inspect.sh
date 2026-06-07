#!/system/bin/sh
#
# fbo_inspect.sh - launch an Android app and summarize what files it
# has mmap'd into its process, broken down by where they live on disk.
#
# Tells you, for a given package:
#   - the full list of mapped files (count + bytes) for /data/app/
#     (the FBO 3.0 pin target)
#   - the same for /system/ and /apex/ (framework, platform-owned)
#   - the same for /data/data/ (runtime data, not in FBO scope)
#   - the top largest mapped files overall
#
# Run on-device as root:
#   sh fbo_inspect.sh com.antutu.ABenchMark
#
# Optional second argument: activity component if cmd package
# resolve-activity cannot find it for you.

PKG=$1
ACT=$2

if [ -z "$PKG" ]; then
    echo "usage: $0 <package> [<activity-component>]" >&2
    exit 1
fi

# --- toolbox compatibility shims --------------------------------------

# stat -c works on modern toybox; wc -c is universal fallback
file_size() {
    s=$(stat -c '%s' "$1" 2>/dev/null)
    if [ -z "$s" ]; then
        s=$(wc -c < "$1" 2>/dev/null)
    fi
    echo "${s:-0}"
}

# Sum sizes of files read from stdin (one path per line)
sum_sizes() {
    total=0
    while read -r f; do
        [ -f "$f" ] || continue
        s=$(file_size "$f")
        total=$((total + s))
    done
    echo "$total"
}

# Bytes -> MB with one decimal
mb() {
    awk -v b="$1" 'BEGIN { printf "%.1f MB", b/1048576 }'
}

# --- resolve activity if not provided ----------------------------------

if [ -z "$ACT" ]; then
    ACT=$(cmd package resolve-activity --brief "$PKG" 2>/dev/null \
          | tail -1 | tr -d '\r')
fi
if [ -z "$ACT" ] || ! echo "$ACT" | grep -q '/'; then
    echo "could not resolve activity for $PKG" >&2
    echo "pass it as second arg, e.g.:" >&2
    echo "  $0 $PKG $PKG/.MainActivity" >&2
    exit 1
fi

echo "package : $PKG"
echo "activity: $ACT"
echo

# --- launch the app and grab its PID -----------------------------------

am force-stop "$PKG"
sleep 1
am start -n "$ACT" >/dev/null 2>&1

# Wait up to 10s for the process to appear and settle
PID=""
for i in 1 2 3 4 5 6 7 8 9 10; do
    PID=$(pidof "$PKG" 2>/dev/null)
    [ -n "$PID" ] && break
    sleep 1
done

if [ -z "$PID" ]; then
    echo "process never appeared for $PKG" >&2
    exit 1
fi

echo "pid     : $PID"

# Give it a bit more time so launch-phase mmaps are complete
sleep 3

# --- snapshot mapped files ---------------------------------------------

MAPS=/data/local/tmp/fbo_inspect_$PID.maps
cat /proc/$PID/maps | awk '{print $NF}' | sort -u \
    | grep -v '^\[' | grep -v '^$' \
    | grep -v '^/memfd' | grep -v '^/dev/' \
    > "$MAPS"

TOTAL_FILES=$(wc -l < "$MAPS")
echo "total mapped files (excluding anon/dev): $TOTAL_FILES"
echo

# --- categorize and report ---------------------------------------------

report_category() {
    label=$1
    pattern=$2

    files=$(grep -E "$pattern" "$MAPS")
    count=$(echo "$files" | grep -c .)
    bytes=$(echo "$files" | sum_sizes)

    echo "[$label]"
    echo "  files : $count"
    echo "  bytes : $bytes ($(mb $bytes))"
}

report_category "/data/app/  (per-app, FBO target)" '^/data/app/'
report_category "/system/ + /apex/  (framework)"   '^/system/|^/apex/'
report_category "/data/data/  (runtime data)"      '^/data/data/'
report_category "/data/dalvik-cache/  (boot OAT)"  '^/data/dalvik-cache/'

echo

# --- list the per-app files explicitly --------------------------------

echo "[/data/app/ files in detail]"
grep '^/data/app/' "$MAPS" | while read -r f; do
    [ -f "$f" ] || continue
    s=$(file_size "$f")
    printf "  %10d  %s\n" "$s" "$f"
done | sort -rn

echo

# --- top largest mapped files overall ---------------------------------

echo "[top 15 largest mapped files]"
while read -r f; do
    [ -f "$f" ] || continue
    s=$(file_size "$f")
    printf "%d %s\n" "$s" "$f"
done < "$MAPS" | sort -rn | head -15 | while read -r s f; do
    printf "  %10d  %s\n" "$s" "$f"
done

echo

# --- cleanup ----------------------------------------------------------

rm -f "$MAPS"

# --- summary ratio ----------------------------------------------------

app_bytes=$(grep '^/data/app/' /proc/$PID/maps | awk '{print $NF}' | sort -u | sum_sizes 2>/dev/null)
fw_bytes=$(grep -E '^/system/|^/apex/' /proc/$PID/maps | awk '{print $NF}' | sort -u | sum_sizes 2>/dev/null)

if [ "$app_bytes" -gt 0 ] && [ "$fw_bytes" -gt 0 ]; then
    pct=$(awk -v a="$app_bytes" -v f="$fw_bytes" \
          'BEGIN { printf "%.1f", 100*a/(a+f) }')
    echo "app share of mapped (app+framework) bytes: ${pct}%"
fi
