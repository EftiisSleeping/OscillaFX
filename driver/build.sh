#!/bin/bash
# Build + ad-hoc sign the OscillaFX Audio driver (bundle Oscilla.driver) with clang only (no Xcode). Override via env vars.
# Upstream BlackHole stays pristine: patches/*.patch are applied to a copy in the build dir.
set -euo pipefail
cd "$(dirname "$0")"

# DRIVER_NAME is the on-disk bundle/executable/factory name. It stays "Oscilla" so an upgrade replaces the
# installed earlier builds driver in place (one HAL plug-in, same bundle id and device UID); only the shown name changed.
DRIVER_NAME="${DRIVER_NAME:-Oscilla}"
DEVICE_NAME="${DEVICE_NAME:-OscillaFX Audio}"
BUNDLE_ID="${BUNDLE_ID:-com.oscilla.audio}"
MANUFACTURER="${MANUFACTURER:-OscillaFX}"
BUNDLE_DISPLAY_NAME="${BUNDLE_DISPLAY_NAME:-OscillaFX Audio}"
ICON_NAME="${ICON_NAME:-OscillaFX}"
CHANNELS="${CHANNELS:-2}"
SAMPLE_RATES="${SAMPLE_RATES:-44100, 48000, 88200, 96000, 176400, 192000}"
VERSION="${VERSION:-0.5.0}"
BUILD_DIR="${BUILD_DIR:-$PWD/build}"
ICON_PNG="${ICON_PNG:-$PWD/../packaging/icon/OscillaFX.png}"
[ -f "$ICON_PNG" ] || ICON_PNG="$PWD/../packaging/icon/Oscilla.png" # pre-rename icon fallback
UPSTREAM=third_party/BlackHole
FACTORY="${DRIVER_NAME}_Create"

[ -f "$UPSTREAM/BlackHole/BlackHole.c" ] || { echo "missing $UPSTREAM (git clone BlackHole into third_party)"; exit 1; }

BUNDLE="$BUILD_DIR/$DRIVER_NAME.driver"
SRC_DIR="$BUILD_DIR/driver-src"
rm -rf "$BUNDLE" "$SRC_DIR"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources" "$SRC_DIR"

cp -R "$UPSTREAM/BlackHole" "$SRC_DIR/"
for p in patches/*.patch; do
  patch -p1 --quiet --forward -d "$SRC_DIR" < "$p"
done

# Defines: kHas_Driver_Name_Format=false -> device name/UID taken verbatim, no "%ich" suffix.
DEFS=(
  -DDEBUG=0
  "-DkDriver_Name=\"$DRIVER_NAME\""
  "-DkDevice_Name=\"$DEVICE_NAME\""
  "-DkDevice2_Name=\"$DEVICE_NAME Mirror\""
  "-DkBox_Name=\"$DEVICE_NAME Box\""
  "-DkPlugIn_BundleID=\"$BUNDLE_ID\""
  "-DkManufacturer_Name=\"$MANUFACTURER\""
  "-DkPlugIn_Icon=\"$ICON_NAME.icns\""
  "-DkFactory_Function=$FACTORY"
  -DkHas_Driver_Name_Format=false
  "-DkNumber_Of_Channels=$CHANNELS"
  "-DkSampleRates=$SAMPLE_RATES"
)

EXECUTABLE="$BUNDLE/Contents/MacOS/$DRIVER_NAME"
clang -arch arm64 -arch x86_64 -mmacosx-version-min=13.0 -O2 -bundle -Wno-format-extra-args \
  "${DEFS[@]}" "$SRC_DIR/BlackHole/BlackHole.c" \
  -framework CoreAudio -framework CoreFoundation -framework Accelerate \
  -o "$EXECUTABLE"
strip -x "$EXECUTABLE" # drop the upstream BlackHole_* local symbol names

sed -e "s|@EXECUTABLE@|$DRIVER_NAME|g" -e "s|@BUNDLE_ID@|$BUNDLE_ID|g" -e "s|@VERSION@|$VERSION|g" \
  -e "s|@FACTORY@|$FACTORY|g" -e "s|@DISPLAY_NAME@|$BUNDLE_DISPLAY_NAME|g" src/Info.plist.in > "$BUNDLE/Contents/Info.plist"
plutil -lint "$BUNDLE/Contents/Info.plist" >/dev/null

# Device icon (Audio MIDI Setup) from the app icon; kPlugIn_Icon names it.
ICONSET="$BUILD_DIR/$ICON_NAME.iconset"
rm -rf "$ICONSET" && mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
  sips -z $size $size "$ICON_PNG" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
  sips -z $((size * 2)) $((size * 2)) "$ICON_PNG" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$BUNDLE/Contents/Resources/$ICON_NAME.icns"

# GPL-3.0: ship the upstream license (and its copyright notice) with the binary.
cp "$UPSTREAM/LICENSE" "$BUNDLE/Contents/Resources/LICENSE"

# Ad-hoc sign in place (never copy a signed bundle over an old one; see install.sh).
codesign --force --sign - --timestamp=none "$BUNDLE"
codesign --verify --strict --verbose=2 "$BUNDLE"
echo "built: $BUNDLE"
