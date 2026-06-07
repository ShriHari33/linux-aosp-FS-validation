#!/system/bin/sh
# fbo_enum.sh — enumerate the static distribution-side files of an
# Android package, suitable for piping into fbo_pin.
#
# Usage:
#   fbo_enum.sh <package>
#
# Example:
#   fbo_enum.sh com.antutu.ABenchMark | xargs ./fbo_pin pin /data/local/tmp/fbo_antutu.rec

PKG=$1
[ -z "$PKG" ] && { echo "usage: $0 <package>" >&2; exit 1; }

ABI=$(getprop ro.product.cpu.abi)
case "$ABI" in
    arm64-v8a)   ISA=arm64 ;;
    armeabi-v7a) ISA=arm    ;;
    x86_64)      ISA=x86_64 ;;
    x86)         ISA=x86    ;;
    *)           ISA=$ABI   ;;
esac

APKS=$(pm path "$PKG" 2>/dev/null | sed 's/^package://')
if [ -z "$APKS" ]; then
    echo "package not installed: $PKG" >&2
    exit 1
fi

DIR=$(dirname "$(echo "$APKS" | head -n1)")

# Base APK + every installed split.
echo "$APKS"

# OAT artifacts (base.odex / base.vdex / base.art and any split equivalents).
ls "$DIR/oat/$ISA"/* 2>/dev/null

# Extracted native libs, if the app uses extractNativeLibs="true".
# The extracted-on-disk directory uses the short ISA name (arm64),
# NOT the full ABI name (arm64-v8a) — the ABI form only appears
# inside the APK's ZIP entries. Apps that mmap libs straight out
# of the APK have no on-disk /lib/ directory; pinning the APK
# already covers those libs.
ls "$DIR/lib/$ISA"/* 2>/dev/null
