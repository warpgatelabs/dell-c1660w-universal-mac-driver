#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build universal (arm64 + x86_64) static copies of PAPPL and its dependencies (OpenSSL,
# libpng, libjpeg-turbo) for macOS 11 or later, into build-deps/universal.
#
# Usage: packaging/macos/build-deps.sh        (run from anywhere; needs cmake, nasm, pkg-config)
#        ONLY_PAPPL=1 packaging/macos/build-deps.sh   rebuild only PAPPL, keeping the libraries
#
# Every download is checked against the SHA-256 below before it is unpacked.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="$ROOT/build-deps"
SRC="$DEPS/src"
UNI="$DEPS/universal"
ARCHS="arm64 x86_64"
export MACOSX_DEPLOYMENT_TARGET=11.0
JOBS="$(sysctl -n hw.ncpu)"

OPENSSL_VER=3.5.9
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"
OPENSSL_SHA=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
PNG_VER=1.6.59
PNG_URL="https://downloads.sourceforge.net/project/libpng/libpng16/$PNG_VER/libpng-$PNG_VER.tar.xz"
PNG_SHA=d80dd2a38a37f803cb9b6ac7b14bd6e74ddc3b654780a8380bdf93523fdb4389
JPEG_VER=3.2.1
JPEG_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JPEG_VER/libjpeg-turbo-$JPEG_VER.tar.gz"
JPEG_SHA=c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f
PAPPL_VER=1.4.12
PAPPL_URL="https://github.com/michaelrsweet/pappl/releases/download/v$PAPPL_VER/pappl-$PAPPL_VER.tar.gz"
PAPPL_SHA=1684c4e06446e9f7d93a39729fa0ba56f07a4007560080fdad7d0e2076a3615f

for tool in cmake nasm pkg-config; do
    command -v "$tool" >/dev/null || { echo "build-deps: $tool is required" >&2; exit 1; }
done

# fetch URL SHA256 -> unpacks into $SRC/<name> (fresh each time), prints that directory
fetch() {
    local url="$1" sha="$2" file="$SRC/$(basename "$1")"
    mkdir -p "$SRC"
    if [ ! -f "$file" ]; then
        echo "build-deps: downloading $url" >&2
        curl -fL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    if [ "$(shasum -a 256 "$file" | cut -d' ' -f1)" != "$sha" ]; then
        echo "build-deps: checksum mismatch for $file" >&2
        exit 1
    fi
    local dir="$SRC/$(basename "$file" | sed -E 's/\.tar\.(gz|xz)$//')"
    rm -rf "$dir"
    tar -xf "$file" -C "$SRC"
    echo "$dir"
}

build_libraries() {
# Per-architecture prefixes, merged into $UNI with lipo afterwards.
rm -rf "$UNI"
for arch in $ARCHS; do rm -rf "$DEPS/$arch"; done

# --- OpenSSL --------------------------------------------------------------------------------
dir="$(fetch "$OPENSSL_URL" "$OPENSSL_SHA")"
for arch in $ARCHS; do
    b="$dir/build-$arch"
    mkdir -p "$b"
    (cd "$b" && "$dir/Configure" "darwin64-$arch-cc" no-shared no-tests no-docs no-apps \
        --prefix="$DEPS/$arch" --libdir=lib --openssldir=/private/etc/ssl \
        "-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" &&
        make -j"$JOBS" build_libs && make install_dev)
done

# --- libpng (system zlib) -------------------------------------------------------------------
dir="$(fetch "$PNG_URL" "$PNG_SHA")"
for arch in $ARCHS; do
    cmake -S "$dir" -B "$dir/build-$arch" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
        -DCMAKE_INSTALL_PREFIX="$DEPS/$arch" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_FRAMEWORK=OFF \
        -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_HARDWARE_OPTIMIZATIONS=OFF
    cmake --build "$dir/build-$arch" -j"$JOBS"
    cmake --install "$dir/build-$arch"
done

# --- libjpeg-turbo --------------------------------------------------------------------------
dir="$(fetch "$JPEG_URL" "$JPEG_SHA")"
for arch in $ARCHS; do
    cmake -S "$dir" -B "$dir/build-$arch" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
        -DCMAKE_SYSTEM_PROCESSOR="$arch" -DCMAKE_INSTALL_PREFIX="$DEPS/$arch" -DCMAKE_INSTALL_LIBDIR=lib \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
    cmake --build "$dir/build-$arch" -j"$JOBS"
    cmake --install "$dir/build-$arch"
done

# --- Merge into one universal prefix --------------------------------------------------------
first="${ARCHS%% *}"
mkdir -p "$UNI/lib/pkgconfig"
for arch in $ARCHS; do
    if ! diff -r "$DEPS/$first/include" "$DEPS/$arch/include" >/dev/null; then
        echo "build-deps: $first and $arch headers differ; cannot merge them" >&2
        diff -r "$DEPS/$first/include" "$DEPS/$arch/include" >&2 || true
        exit 1
    fi
done
cp -R "$DEPS/$first/include" "$UNI/include"
for lib in libssl.a libcrypto.a libpng16.a libjpeg.a; do
    inputs=()
    for arch in $ARCHS; do inputs+=("$DEPS/$arch/lib/$lib"); done
    lipo -create "${inputs[@]}" -output "$UNI/lib/$lib"
done
ln -sf libpng16.a "$UNI/lib/libpng.a"
for pc in openssl libssl libcrypto libpng16 libpng libjpeg; do
    [ -f "$DEPS/$first/lib/pkgconfig/$pc.pc" ] || continue
    sed "s|$DEPS/$first|$UNI|g" "$DEPS/$first/lib/pkgconfig/$pc.pc" >"$UNI/lib/pkgconfig/$pc.pc"
done
# libpng16.pc requires zlib, which macOS provides without a .pc file.
cat >"$UNI/lib/pkgconfig/zlib.pc" <<EOF
Name: zlib
Description: zlib from the macOS SDK
Version: 1.2.12
Libs: -lz
Cflags:
EOF

# Licenses of everything linked into the binary, for the installer.
LIC="$UNI/licenses"
mkdir -p "$LIC"
cp "$SRC/openssl-$OPENSSL_VER/LICENSE.txt" "$LIC/OpenSSL-LICENSE.txt"
cp "$SRC/libpng-$PNG_VER/LICENSE" "$LIC/libpng-LICENSE.txt"
cp "$SRC/libjpeg-turbo-$JPEG_VER/LICENSE.md" "$LIC/libjpeg-turbo-LICENSE.md"
cp "$SRC/libjpeg-turbo-$JPEG_VER/README.ijg" "$LIC/libjpeg-turbo-README.ijg"
}

if [ "${ONLY_PAPPL:-0}" = 1 ]; then
    [ -f "$UNI/lib/libssl.a" ] || { echo "build-deps: ONLY_PAPPL=1 needs a full build first" >&2; exit 1; }
else
    build_libraries
fi
LIC="$UNI/licenses"

# --- PAPPL, universal in one pass, against only the libraries above ------------------------
dir="$(fetch "$PAPPL_URL" "$PAPPL_SHA")"
cp "$dir/LICENSE" "$LIC/PAPPL-LICENSE.txt"
cp "$dir/NOTICE" "$LIC/PAPPL-NOTICE.txt"
(
    cd "$dir"
    export PKG_CONFIG_LIBDIR="$UNI/lib/pkgconfig" PKG_CONFIG_PATH=
    export CFLAGS="-arch arm64 -arch x86_64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
    export LDFLAGS="$CFLAGS"
    # The socket directory must match the service's C1660W_STATE_DIR (see the launchd plist):
    # command-line sub-commands find the system service through a socket there.
    ./configure --prefix="$UNI" --disable-shared --with-dnssd=mdnsresponder --with-tls=openssl \
        --enable-libjpeg --enable-libpng --disable-libusb \
        --with-papplsockdir="/Library/Application Support/c1660w-universal-mac-driver"
    make -j"$JOBS" -C pappl
    make -C pappl install
)

echo
echo "build-deps: done. Universal prefix: $UNI"
for lib in "$UNI"/lib/*.a; do printf '  %-16s %s\n' "$(basename "$lib")" "$(lipo -archs "$lib")"; done
