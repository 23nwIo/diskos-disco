#!/bin/bash
# build-rsync-static.sh - build a static little-endian MIPS rsync for the Disc (server side of
# `rsync -e ssh`). It is NOT part of the flashed image; tools/disc-ssh-setup.sh --rsync copies it to
# /usr/data/sshd/bin/rsync. See docs/SSH_ACCESS.md.
#
# Source: rsync 3.4.1 (GPL-3.0-or-later), fetched by git tag and pinned to its commit hash. Anyone you
# give the binary to must be able to get this source too (GPLv3 s6) - hand the tarball written next to it.
#
# Compiler (first match wins):
#   CROSS=<prefix>   e.g. the project toolchain from ui/Dockerfile: /opt/mipsel-n2008-musl-cross/bin/mipsel-linux-musl-
#                    (MIPS32r2, FP64, NaN2008 - the stock ABI; preferred)
#   otherwise mipsel-linux-gnu-  (Debian/Ubuntu: apt install gcc-mipsel-linux-gnu). Static glibc, MIPS32r2,
#                    FPXX/legacy NaN: runs on the Disc (rsync does almost no floating point). Link-time
#                    getpwnam/gethostbyname NSS warnings are expected; with -rt (no owner mapping) and an
#                    ssh transport they are not used.
#
# Features off: OpenSSL, xxhash, zstd, lz4, iconv, ACLs, xattrs (exFAT SD has none); bundled zlib + popt.
# Output: build/out/rsync-mipsel (stripped) + build/out/rsync-3.4.1-source.tar.gz
# Optional check without a device: qemu-mipsel-static build/out/rsync-mipsel --version
set -euo pipefail
cd "$(dirname "$0")"
OUT="$(pwd)/out"; mkdir -p "$OUT"
TAG=v3.4.1
COMMIT=3305a7a063ab0167cab5bf7029da53abaa9fdb6e

if [ -n "${CROSS:-}" ]; then P="$CROSS"; ARCH="-march=mips32r2 -mnan=2008 -mfp64"
elif command -v mipsel-linux-gnu-gcc >/dev/null; then P=mipsel-linux-gnu-; ARCH="-march=mips32r2"
else echo "no MIPS cross compiler: set CROSS=... or install gcc-mipsel-linux-gnu" >&2; exit 1; fi

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
git clone -q --depth 1 -b "$TAG" https://github.com/RsyncProject/rsync.git "$WORK/rsync"
[ "$(git -C "$WORK/rsync" rev-parse HEAD)" = "$COMMIT" ] || { echo "tag $TAG is not commit $COMMIT - refusing" >&2; exit 1; }
git -C "$WORK/rsync" archive --prefix=rsync-3.4.1/ -o "$OUT/rsync-3.4.1-source.tar.gz" HEAD

cd "$WORK/rsync"
CC="${P}gcc" CFLAGS="-Os $ARCH" LDFLAGS="-static" ./configure --host="$("${P}gcc" -dumpmachine)" \
    --disable-openssl --disable-xxhash --disable-zstd --disable-lz4 --disable-md2man --disable-iconv \
    --disable-acl-support --disable-xattr-support --with-included-zlib --with-included-popt >configure.log 2>&1 \
    || { tail -30 configure.log; exit 1; }
make -j"$(nproc 2>/dev/null || echo 2)" rsync >make.log 2>&1 || { tail -30 make.log; exit 1; }
"${P}strip" -o "$OUT/rsync-mipsel" rsync
file "$OUT/rsync-mipsel" 2>/dev/null || true
sha256sum "$OUT/rsync-mipsel" "$OUT/rsync-3.4.1-source.tar.gz" 2>/dev/null || shasum -a 256 "$OUT/rsync-mipsel"
