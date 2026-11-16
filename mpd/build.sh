#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build MPD as a static MIPS binary for the own player's audio core (docs/MQ_PLAYER_REPLACEMENT.md, Jo 2026-11-15).
#
#   mpd/build.sh <toolchain-with-c++> <work-dir>
#
# <toolchain-with-c++>: the diskOS toolchain (ui/toolchain/mcm-config.mak) built with --enable-languages=c,c++ and
# libatomic; see "C++ compiler" below. Sources are cloned from GitHub at pinned tags into <work-dir>/src, the libraries
# are installed into <work-dir>/sysroot, the stripped binary is <work-dir>/mpd. Needs: git, meson (pip), ninja, cmake,
# autoconf/automake/libtool, nasm, pkg-config and the boost headers (libboost-dev, header-only use).
#
# C++ compiler (when the toolchain has only C): clone gcc-mirror/gcc at releases/gcc-11.2.0, apply
# musl-cross-make's patches/gcc-11.2.0/*.diff (commit 227df8b9...), symlink <tc>/mipsel-linux-musl/usr -> ., then
#   configure --target=mipsel-linux-musl --prefix=<tc> --with-sysroot=<tc>/mipsel-linux-musl --enable-languages=c,c++
#     --with-arch=mips32r2 --with-nan=2008 --with-fp-32=64 --with-odd-spreg-32 --disable-multilib --disable-nls
#     --disable-libquadmath --disable-libsanitizer --disable-libgomp --disable-libitm --disable-libssp --enable-tls
#     --enable-initfini-array --enable-libstdcxx-time=rt --disable-bootstrap --disable-werror
#   make all-gcc all-target-libgcc all-target-libstdc++-v3 all-target-libatomic + the matching install-* targets
# and move the shared libstdc++/libatomic (.so) out of <tc>/mipsel-linux-musl/lib, so the static link takes the .a.
set -euo pipefail
TC=$(realpath "$1"); W=$(realpath -m "$2")
SYS=$W/sysroot; SRC=$W/src
export PATH=$TC/bin:$PATH
CROSS=mipsel-linux-musl
export CFLAGS="-Os -pipe -ffunction-sections -fdata-sections"
mkdir -p "$SRC" "$SYS"

fetch(){ [ -d "$SRC/$2" ] || git clone -q --depth 1 --branch "$3" --recurse-submodules --shallow-submodules "https://github.com/$1" "$SRC/$2"; }
fetch MusicPlayerDaemon/MPD MPD v0.23.17
fetch alsa-project/alsa-lib alsa-lib v1.2.12
fetch xiph/flac flac 1.4.3
fetch FFmpeg/FFmpeg FFmpeg n6.1.2
fetch fmtlib/fmt fmt 10.2.1
fetch Mbed-TLS/mbedtls mbedtls v3.6.2
fetch curl/curl curl curl-8_11_1

# alsa-lib: the Disc's configuration is in /usr/share/alsa (checked 2026-11-15)
(cd "$SRC/alsa-lib" && autoreconf -fi && ./configure --host=$CROSS --prefix=/usr --enable-static --disable-shared \
   --disable-python --disable-topology --disable-ucm --with-configdir=/usr/share/alsa --with-plugindir=/usr/lib/alsa-lib \
   && make -j"$(nproc)" && make install DESTDIR="$SYS")

cmake_lib(){   # <dir> <cmake options...>
    local d=$1; shift
    rm -rf "$d/b" && mkdir "$d/b" && (cd "$d/b" && cmake .. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=mips \
        -DCMAKE_C_COMPILER=$CROSS-gcc -DCMAKE_CXX_COMPILER=$CROSS-g++ -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_SHARED_LIBS=OFF "$@" && make -j"$(nproc)" && make install DESTDIR="$SYS")
}
cmake_lib "$SRC/flac" -DWITH_OGG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF \
    -DBUILD_CXXLIBS=OFF -DINSTALL_MANPAGES=OFF -DWITH_STACK_PROTECTOR=OFF -DENABLE_MULTITHREADING=OFF
cmake_lib "$SRC/fmt" -DFMT_DOC=OFF -DFMT_TEST=OFF
# web radio: libcurl (HTTP/HTTPS only) over mbedTLS for MPD's curl input plugin (ICY titles included). No CA bundle is
# compiled in: the player names one in mpd.conf (engine.c, "cacert").
cmake_lib "$SRC/mbedtls" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
cmake_lib "$SRC/curl" -DBUILD_CURL_EXE=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
    -DBUILD_TESTING=OFF -DHTTP_ONLY=ON -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF \
    -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_GSSAPI=OFF \
    -DCURL_DISABLE_LDAP=ON -DENABLE_THREADED_RESOLVER=ON -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=OFF \
    -DMBEDTLS_INCLUDE_DIR="$SYS/usr/include" -DMBEDTLS_LIBRARY="$SYS/usr/lib/libmbedtls.a" \
    -DMBEDX509_LIBRARY="$SYS/usr/lib/libmbedx509.a" -DMBEDCRYPTO_LIBRARY="$SYS/usr/lib/libmbedcrypto.a"
# a static link needs curl's own dependencies after -lcurl; MPD's meson asks pkg-config without --static
sed -i 's/^Libs: \(.*\)-lcurl$/Libs: \1-lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lpthread/' "$SYS/usr/lib/pkgconfig/libcurl.pc"
grep -q -- '-lcurl -lmbedtls' "$SYS/usr/lib/pkgconfig/libcurl.pc" || { echo "libcurl.pc: Libs line not patched" >&2; exit 1; }

# FFmpeg: only the demuxers/decoders a music player needs (FLAC goes through libFLAC, DSF/DFF through MPD itself)
(cd "$SRC/FFmpeg" && ./configure --enable-cross-compile --cross-prefix=$CROSS- --arch=mips --cpu=mips32r2 --target-os=linux \
   --prefix=/usr --disable-everything --disable-programs --disable-doc --disable-network --disable-avdevice \
   --disable-swscale --disable-postproc --disable-avfilter --disable-swresample --enable-static --disable-shared \
   --disable-mipsdsp --disable-mipsdspr2 --disable-msa --disable-mmi --disable-mipsfpu --enable-small \
   --enable-protocol=file --enable-demuxer=aac,mov,ape,wv,ogg,matroska,mp3,wav,aiff,flac,dsf,iff,asf \
   --enable-decoder=aac,alac,ape,wavpack,vorbis,opus,mp3,mp3float,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s16be,pcm_s24be,pcm_f32le,flac,wmav1,wmav2,wmalossless,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar \
   --enable-parser=aac,mpegaudio,flac,opus,vorbis --extra-cflags="$CFLAGS" && make -j"$(nproc)" && make install DESTDIR="$SYS")

# boost: header-only use; meson's boost lookup does not find headers in a cross sysroot, so MPD gets a plain dependency
mkdir -p "$SYS/usr/include" && cp -r /usr/include/boost "$SYS/usr/include/"
# our patches to MPD (patches/*.patch, applied once)
for p in "$(dirname "$(realpath "$0")")"/patches/*.patch; do
    (cd "$SRC/MPD" && { git apply --check "$p" 2>/dev/null && git apply "$p"; } || git apply --reverse --check "$p")
done
sed -i "s/^boost_dep = dependency('boost', version: '>= 1.58')/boost_dep = declare_dependency(version: '1.83')/" "$SRC/MPD/meson.build"

cat > "$W/cross.ini" <<INI
[binaries]
c = '$TC/bin/$CROSS-gcc'
cpp = '$TC/bin/$CROSS-g++'
ar = '$TC/bin/$CROSS-ar'
strip = '$TC/bin/$CROSS-strip'
pkgconfig = 'pkg-config'
[properties]
pkg_config_libdir = '$SYS/usr/lib/pkgconfig'
sys_root = '$SYS'
[built-in options]
c_args = ['-Os','-ffunction-sections','-fdata-sections','-I$SYS/usr/include']
cpp_args = ['-Os','-ffunction-sections','-fdata-sections','-I$SYS/usr/include']
c_link_args = ['-static','-Wl,--gc-sections','-L$SYS/usr/lib']
cpp_link_args = ['-static','-Wl,--gc-sections','-L$SYS/usr/lib']
[host_machine]
system = 'linux'
cpu_family = 'mips'
cpu = 'mips32r2'
endian = 'little'
INI

# no database (the library stays diskOS's own), no TCP needed by the player but kept for tests, local socket for file://
(cd "$SRC/MPD" && rm -rf b && meson setup b --cross-file "$W/cross.ini" --buildtype=minsize -Db_ndebug=true \
   -Dauto_features=disabled -Ddatabase=false -Dneighbor=false -Dinotify=false -Ddaemon=false -Dtest=false \
   -Ddsd=true -Dcue=true -Dalsa=enabled -Dflac=enabled -Dffmpeg=enabled -Dcurl=enabled -Dpipe=true -Dfifo=false -Dhttpd=false \
   -Drecorder=false -Dtcp=true -Dlocal_socket=true -Dio_uring=disabled -Dzeroconf=disabled -Dwrap_mode=nodownload \
   && ninja -C b)
$CROSS-strip -o "$W/mpd" "$SRC/MPD/b/mpd"
# (into a variable: grep -q ends early and readelf's SIGPIPE would fail the pipe under pipefail)
abi=$($CROSS-readelf -A "$W/mpd"); grep -q "64-bit FPU" <<<"$abi" || { echo "wrong FP ABI" >&2; exit 1; }
sha256sum "$W/mpd"
