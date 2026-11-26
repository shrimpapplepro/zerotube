#!/usr/bin/env bash
# Builds the device .deb natively on an arm64 Debian trixie host, inside a
# Docker image that has the CardputerZero build dependencies plus the FFmpeg and
# PulseAudio dev packages, and copies it to dist/.
#   HOST=user@build-host IMAGE=<image> tools/build-device.sh
# HOST/IMAGE/REMOTE can also live in tools/local.env (gitignored).
set -euo pipefail
LOCAL_ENV="$(cd "$(dirname "$0")" && pwd)/local.env"
# shellcheck disable=SC1090
[ -f "$LOCAL_ENV" ] && . "$LOCAL_ENV"
: "${HOST:?set HOST=user@build-host (or put it in tools/local.env)}"
: "${IMAGE:?set IMAGE=<docker image with the build deps> (or put it in tools/local.env)}"
REMOTE="${REMOTE:-zerotube-src}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

echo "==> sync to $HOST:~/$REMOTE"
ssh "$HOST" "mkdir -p $REMOTE"
rsync -a --delete --exclude build/ --exclude .git/ --exclude dist/ --exclude .cache/ --exclude .DS_Store \
    "$SRC/" "$HOST:$REMOTE/"

echo "==> build in $IMAGE"
ssh "$HOST" "cd $REMOTE && docker run --rm -u \$(id -u):\$(id -g) -e HOME=/tmp -v \$PWD:/src -w /src $IMAGE sh -ec '
    cmake --preset cp0-native -DCM0_SDK_ROOT=/ >/dev/null
    cmake --build --preset cp0-native-rel -j4 2>&1 | grep -E \"error|warning: unused|Playback\" || true
    test -x build/cp0-native/Release/zerotube || test -x build/cp0-native/zerotube
    rm -rf dist
    cpack --config build/cp0-native/CPackConfig.cmake -G DEB -C Release -B dist >/dev/null
    ls dist/*.deb'"

mkdir -p "$SRC/dist"
rsync -a "$HOST:$REMOTE/dist/*.deb" "$SRC/dist/"
ls -la "$SRC"/dist/*.deb
