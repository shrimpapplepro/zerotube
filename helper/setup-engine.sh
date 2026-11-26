#!/bin/sh
# SPDX-FileCopyrightText: 2026 ZeroTube contributors
# SPDX-License-Identifier: MIT
#
# Installs or updates ZeroTube's YouTube engine (yt-dlp) for the current user,
# in ~/.local/share/zerotube/venv. No root: YouTube changes often enough that
# Debian's yt-dlp is always too old, so the app keeps its own up to date.
# Prints "STEP <text>" lines the app shows as progress; exit 0 = done.
set -eu
DIR="${XDG_DATA_HOME:-$HOME/.local/share}/zerotube"
VENV="$DIR/venv"
mkdir -p "$DIR"

if [ ! -x "$VENV/bin/python" ]; then
    echo "STEP Creating Python environment"
    python3 -m venv "$VENV"
fi

echo "STEP Downloading yt-dlp"
"$VENV/bin/python" -m pip install --disable-pip-version-check --no-input -q --upgrade "yt-dlp[default]"

VERSION=$("$VENV/bin/python" -c 'from yt_dlp.version import __version__; print(__version__)')
echo "STEP yt-dlp $VERSION ready"
