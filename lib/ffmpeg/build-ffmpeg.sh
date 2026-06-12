#!/bin/bash
#
# Cross-compile FFmpeg for Nintendo 3DS (devkitARM)
#
# Run inside the devkitPro Docker container:
#   docker run --rm -v $PWD:/src -w /src devkitpro/devkitarm bash lib/ffmpeg/build-ffmpeg.sh
#
# Or from host:
#   ./lib/ffmpeg/build-ffmpeg.sh docker
#
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
FFMPEG_SRC="/tmp/ffmpeg-build/src"
FFMPEG_PREFIX="/tmp/ffmpeg-build/install"
# Pinned commit of the ThirdTube FFmpeg fork. The CI cache key hashes this
# script, so bumping the pin automatically invalidates cached .a files.
FFMPEG_PIN="306994b887d484eb9e0a9aa95b8d7825ce4efec1"

# If "docker" argument, re-run inside container
if [ "$1" = "docker" ]; then
    echo "Building FFmpeg in Docker..."
    docker run --rm \
        -v "$PROJECT_DIR:/src/jellyfin-3ds" \
        -w /src/jellyfin-3ds \
        devkitpro/devkitarm \
        bash lib/ffmpeg/build-ffmpeg.sh
    exit $?
fi

# Verify we're in devkitPro environment
if [ -z "$DEVKITARM" ]; then
    echo "ERROR: DEVKITARM not set. Run inside devkitPro Docker container."
    exit 1
fi

echo "=== FFmpeg Cross-Compile for 3DS ==="
echo "DEVKITARM: $DEVKITARM"
echo "DEVKITPRO: $DEVKITPRO"

# Install build deps (python3 is needed for the configure patch below).
# update may fail transiently (stale mirrors) — install failing is fatal.
apt-get update -qq || true
apt-get install -y -qq git texinfo python3

# Clone FFmpeg (ThirdTube's fork has the pthreads patch) at the pinned commit
if [ ! -d "$FFMPEG_SRC" ]; then
    echo "=== Cloning FFmpeg (pin $FFMPEG_PIN) ==="
    git clone https://github.com/windows-server-2003/FFmpeg.git "$FFMPEG_SRC"
fi

cd "$FFMPEG_SRC"
git checkout --quiet "$FFMPEG_PIN"

# Clean any previous failed build
make distclean 2>/dev/null || true

# Apply the enum-sizing patch if not already applied
if ! grep -q "AV_SAMPLE_FMT_FORCE_INT" libavutil/samplefmt.h 2>/dev/null; then
    echo "=== Applying devkitARM enum patch ==="
    # devkitARM uses -fshort-enums by default. FFmpeg enums must be int-sized
    # to avoid ABI mismatches with the pre-compiled libctru.
    sed -i 's/AV_SAMPLE_FMT_NB\b/AV_SAMPLE_FMT_NB, AV_SAMPLE_FMT_FORCE_INT = 0x7FFFFFFF/' \
        libavutil/samplefmt.h
fi

# Patch os_support.c: suppress format warnings from uint32_t being unsigned long on ARM
if ! grep -q "3DS patched" libavformat/os_support.c 2>/dev/null; then
    echo "=== Patching os_support.c ==="
    sed -i '1s/^/\/* 3DS patched *\/\n#pragma GCC diagnostic ignored "-Wformat"\n#pragma GCC diagnostic ignored "-Wformat-security"\n/' libavformat/os_support.c
fi

# Export toolchain early
export PATH="$DEVKITARM/bin:$PATH"

# Patch configure to force-enable pthreads (3DS provides them via fake_pthread)
# The configure script tries to compile+link test programs with -lpthread,
# which fails during cross-compilation. We replace the entire pthreads
# check block with a forced enable.
echo "=== Patching configure for pthreads ==="
python3 -c "
import re
with open('configure', 'r') as f:
    text = f.read()

# Replace the pthreads detection block with forced enable
old = r'if ! disabled pthreads && ! enabled w32threads && ! enabled os2threads; then'
idx = text.find(old)
if idx >= 0:
    # Find the matching fi (count nesting)
    depth = 1
    pos = idx + len(old)
    while depth > 0 and pos < len(text):
        line = text[pos:text.find(chr(10), pos)+1]
        stripped = line.strip()
        if stripped.startswith('if ') or stripped.startswith('if['):
            depth += 1
        elif stripped == 'fi':
            depth -= 1
        pos = text.find(chr(10), pos) + 1
    # Replace the entire block
    replacement = '''# 3DS: force-enable pthreads (provided by fake_pthread.c at link time)
enable pthreads
'''
    text = text[:idx] + replacement + text[pos:]

# Skip sem_timedwait check
text = text.replace('check_builtin sem_timedwait', '# 3DS skip: check_builtin sem_timedwait')

with open('configure', 'w') as f:
    f.write(text)
print('Configure patched successfully')
"

# Ensure fake_pthread header is available
FAKE_PTHREAD_DIR="/tmp/ffmpeg-build/fake_pthread"
mkdir -p "$FAKE_PTHREAD_DIR"
cat > "$FAKE_PTHREAD_DIR/pthread.h" << 'PTHEOF'
/* pthread.h shim for FFmpeg on 3DS
 *
 * This header is placed FIRST in the include path (-I) so it takes
 * priority over newlib's pthread.h. We block newlib's _pthreadtypes.h
 * from being included and define our own simple types that match
 * the fake_pthread.c implementation. */
#ifndef FAKE_PTHREAD_H
#define FAKE_PTHREAD_H

/* Include newlib's pthread types for all structs and typedefs */
#include <sys/types.h>
#include <sys/_pthreadtypes.h>

/* newlib uses underscore-prefixed names; FFmpeg expects standard names */
#ifndef PTHREAD_MUTEX_INITIALIZER
#define PTHREAD_MUTEX_INITIALIZER _PTHREAD_MUTEX_INITIALIZER
#endif
#ifndef PTHREAD_COND_INITIALIZER
#define PTHREAD_COND_INITIALIZER _PTHREAD_COND_INITIALIZER
#endif
#ifndef PTHREAD_ONCE_INIT
#define PTHREAD_ONCE_INIT _PTHREAD_ONCE_INIT
#endif

int pthread_create(pthread_t *t, const pthread_attr_t *a, void*(*fn)(void*), void *arg);
int pthread_join(pthread_t t, void **retval);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);

int pthread_once(pthread_once_t *once, void (*fn)(void));

long sysconf(int name);
#define _SC_NPROCESSORS_ONLN 84

#endif
PTHEOF

echo "=== Configuring FFmpeg ==="
mkdir -p "$FFMPEG_PREFIX"

./configure \
    --enable-cross-compile \
    --cross-prefix=arm-none-eabi- \
    --prefix="$FFMPEG_PREFIX" \
    --cpu=armv6k \
    --arch=arm \
    --target-os=linux \
    --extra-cflags="-mfloat-abi=hard -mtune=mpcore -mtp=cp15 -D_POSIX_THREADS -fno-short-enums -Wno-format -Wno-format-security -I$FAKE_PTHREAD_DIR -I$DEVKITPRO/libctru/include -O2 -ffunction-sections" \
    --extra-ldflags="-mfloat-abi=hard" \
    --disable-filters \
    --disable-devices \
    --disable-bsfs \
    --disable-parsers \
    --disable-hwaccels \
    --disable-debug \
    --disable-programs \
    --disable-avdevice \
    --disable-postproc \
    --disable-decoders \
    --disable-demuxers \
    --disable-encoders \
    --disable-muxers \
    --disable-asm \
    --disable-protocols \
    --disable-network \
    --disable-swscale \
    --enable-pthreads \
    --enable-inline-asm \
    --enable-vfp \
    --enable-armv5te \
    --enable-armv6 \
    --enable-decoder="aac,h264" \
    --enable-demuxer="mpegts,mov" \
    --enable-parser="h264,aac" \
    --enable-protocol="file" \
    --enable-filter="aformat,aresample,anull" \
    --enable-swresample

echo "=== Building FFmpeg (this takes a while) ==="
make -j$(nproc) 2>&1 | tail -20
make install 2>&1 | tail -10

echo "=== Copying results ==="
# Copy back to wherever this script lives (works in the local Docker
# mount at /src/jellyfin-3ds and in CI checkouts alike)
DEST="$SCRIPT_DIR"
cp -r "$FFMPEG_PREFIX/lib/"*.a "$DEST/"
cp -r "$FFMPEG_PREFIX/include/" "$DEST/include/"

echo ""
echo "=== Build Complete ==="
ls -lh "$DEST/"*.a
echo ""
echo "Headers in: $DEST/include/"
ls "$DEST/include/"
