#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the macOS installer: a universal, signed and notarized c1660w-universal-mac-driver-VERSION.pkg
# in build/. Run packaging/macos/build-deps.sh once first.
#
# Environment:
#   SIGN=0           build unsigned (no Developer ID needed); implies NOTARIZE=0
#   NOTARIZE=0       sign but skip notarization
#   APP_IDENTITY     codesign identity   (default: Developer ID Application: Warpgate LLC (NBXJG6586W))
#   PKG_IDENTITY     productbuild identity (default: Developer ID Installer: Warpgate LLC (NBXJG6586W))
#   NOTARY_PROFILE   notarytool keychain profile (default: hbpl-notary)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/packaging/macos"
UNI="$ROOT/build-deps/universal"
OUT="$ROOT/build/pkg"
ID=com.warpgate.c1660w-universal-mac-driver
SIGN="${SIGN:-1}"
NOTARIZE="${NOTARIZE:-$SIGN}"
APP_IDENTITY="${APP_IDENTITY:-Developer ID Application: Warpgate LLC (NBXJG6586W)}"
PKG_IDENTITY="${PKG_IDENTITY:-Developer ID Installer: Warpgate LLC (NBXJG6586W)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-hbpl-notary}"
VERSION="$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' "$ROOT/app/c1660w-universal-mac-driver.c")"

[ -f "$UNI/lib/pkgconfig/pappl.pc" ] || { echo "build-pkg: run packaging/macos/build-deps.sh first" >&2; exit 1; }

# --- Universal binary, linked only against build-deps and the system ------------------------
BIN="$ROOT/build/universal/c1660w-universal-mac-driver"
rm -f "$BIN"
env CFLAGS="-O2 -arch arm64 -arch x86_64 -mmacosx-version-min=11.0" \
    PKG_CONFIG_LIBDIR="$UNI/lib/pkgconfig" PKG_CONFIG_PATH= \
    make -C "$ROOT" app BUILD=build/universal PAPPL_PREFIX="$UNI"

archs="$(lipo -archs "$BIN")"
[ "$archs" = "x86_64 arm64" ] || [ "$archs" = "arm64 x86_64" ] || { echo "build-pkg: architectures are '$archs'" >&2; exit 1; }
# Library lines are indented; the unindented lines are the per-architecture headers.
if otool -L "$BIN" | grep -E '^[[:space:]]' | grep -v -E '^[[:space:]]+(/usr/lib/|/System/Library/)'; then
    echo "build-pkg: the binary links a library outside the system (above)" >&2
    exit 1
fi
for arch in arm64 x86_64; do
    minos="$(vtool -arch "$arch" -show-build "$BIN" | awk '/minos/ {print $2}')"
    [ "$minos" = 11.0 ] || { echo "build-pkg: $arch minimum macOS is $minos, expected 11.0" >&2; exit 1; }
done

# --- Payload --------------------------------------------------------------------------------
rm -rf "$OUT"
PAYLOAD="$OUT/root"
SHARE="$PAYLOAD/usr/local/share/c1660w-universal-mac-driver"
mkdir -p "$PAYLOAD/usr/local/bin" "$PAYLOAD/Library/LaunchDaemons" "$SHARE/licenses"
install -m 755 "$BIN" "$PAYLOAD/usr/local/bin/c1660w-universal-mac-driver"
install -m 644 "$HERE/$ID.plist" "$PAYLOAD/Library/LaunchDaemons/$ID.plist"
plutil -lint "$PAYLOAD/Library/LaunchDaemons/$ID.plist" >/dev/null
install -m 755 "$HERE/uninstall.sh" "$SHARE/uninstall.sh"
install -m 644 "$ROOT/LICENSE" "$ROOT/NOTICE" "$ROOT/README.md" "$SHARE/"
install -m 644 "$UNI"/licenses/* "$SHARE/licenses/"
SCRIPTS="$OUT/scripts"
mkdir -p "$SCRIPTS"
install -m 755 "$HERE/scripts/preinstall" "$HERE/scripts/postinstall" "$SCRIPTS/"

if [ "$SIGN" = 1 ]; then
    codesign --force --timestamp --options runtime --identifier "$ID" --sign "$APP_IDENTITY" \
        "$PAYLOAD/usr/local/bin/c1660w-universal-mac-driver"
    codesign --verify --strict --verbose=2 "$PAYLOAD/usr/local/bin/c1660w-universal-mac-driver"
fi

# --- Package --------------------------------------------------------------------------------
pkgbuild --root "$PAYLOAD" --scripts "$SCRIPTS" --identifier "$ID" --version "$VERSION" \
    --install-location / --ownership recommended "$OUT/component.pkg"

# pkgbuild stores extended attributes as AppleDouble "._" entries, and the installer would
# create those as real files (/._usr, /._Library, ...). macOS adds com.apple.provenance to new
# files and it cannot be removed, so rebuild the payload, file list and scripts without them.
EXP="$OUT/component-expanded"
pkgutil --expand "$OUT/component.pkg" "$EXP"
lsbom "$EXP/Bom" | grep -v -E '(^|/)\._' >"$OUT/bom.txt"
rm -f "$EXP/Bom" "$OUT/component.pkg"
mkbom -i "$OUT/bom.txt" "$EXP/Bom"
cpio_of() {
    (cd "$1" && tar --no-mac-metadata --format cpio --uid 0 --gid 0 --uname root --gname wheel -cf - .) | gzip -9
}
cpio_of "$PAYLOAD" >"$EXP/Payload"
# pkgutil --expand unpacks the scripts into a folder (which --flatten archives again), with
# the "._" entries as plain files.
find "$EXP/Scripts" -name '._*' -delete
sed -i '' "s/numberOfFiles=\"[0-9]*\"/numberOfFiles=\"$(wc -l <"$OUT/bom.txt" | tr -d ' ')\"/" "$EXP/PackageInfo"
pkgutil --flatten "$EXP" "$OUT/component.pkg"

cat >"$OUT/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>C1660w Universal Mac Driver $VERSION</title>
    <options customize="never" require-scripts="true" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <os-version min="11.0"/>
    <license file="LICENSE"/>
    <choices-outline><line choice="default"><line choice="$ID"/></line></choices-outline>
    <choice id="default"/>
    <choice id="$ID" visible="false"><pkg-ref id="$ID"/></choice>
    <pkg-ref id="$ID" version="$VERSION" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
EOF
mkdir -p "$OUT/resources"
cp "$ROOT/LICENSE" "$OUT/resources/LICENSE"

PKG="$ROOT/build/c1660w-universal-mac-driver-$VERSION.pkg"
sign_args=()
[ "$SIGN" = 1 ] && sign_args=(--sign "$PKG_IDENTITY" --timestamp)
productbuild --distribution "$OUT/distribution.xml" --package-path "$OUT" \
    --resources "$OUT/resources" "${sign_args[@]+"${sign_args[@]}"}" "$PKG"
if pkgutil --payload-files "$PKG" | grep -E '(^|/)\._'; then
    echo "build-pkg: AppleDouble entries (above) are still in the package" >&2
    exit 1
fi

if [ "$NOTARIZE" = 1 ]; then
    xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$PKG"
    spctl --assess --type install --verbose=2 "$PKG"
fi

echo "build-pkg: $PKG"
