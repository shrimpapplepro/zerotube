# ZeroTube: YouTube on the M5 CardputerZero

<img src="assets/images/zerotube.png" width="96" align="right" alt="ZeroTube icon">

A small YouTube player for the CardputerZero's 320x170 screen, in YouTube's own dark colours.
Three tabs (**For you**, **Subs**, **Account**), search, a list of videos with tiny
thumbnails, titles and channel names, and one key to start watching. No comments, no
shorts shelf, nothing else.

ZeroTube is an unofficial personal app. It is not made by or affiliated with Google or YouTube.

## Keys

| Where | Key | Does |
| --- | --- | --- |
| Lists | `1` `2` `3`, or `Z`/`C` (←/→) | For you / Subs / Account |
| | `F`/`X` (↑/↓) | move |
| | `OK` | play the video now |
| | `S` | search (type, `OK` to search, `Esc` to cancel) |
| | `R` | reload the list |
| | `Esc` | leave search results / a list opened from Account |
| Player | `OK` or `Space` | pause / play (at the end: replay) |
| | `Z`/`C` (←/→) | back / forward 10 s |
| | `F`/`X` (↑/↓) | volume |
| | `Esc` | back to the list |
| Account | `OK` | open History, Watch later, Liked videos; sign in or out |
| | `U` | update yt-dlp |

Hold `Esc` to leave the app, as everywhere on the CardputerZero.

## Signing in

For you, Subs and the Account lists need your YouTube account; search and playback do not.
Account > *Sign in* shows a QR code and an address such as `http://192.168.1.20:8791/`. Open it
on a phone or computer on the same network (or over Tailscale) and send a `cookies.txt` exported
from youtube.com (Netscape format, for example with the "Get cookies.txt LOCALLY" extension).
Export it from a private window and close that window afterwards, or YouTube rotates the
cookies and the sign-in stops working. The page is only up while the QR code is on screen.

The cookies are kept in `~/.config/zerotube/cookies.txt` (mode 0600, YouTube and Google lines
only). *Sign out* deletes the file.

## How it works

- **The app** (C++, LVGL 9.5, built on the CardputerZero Template) draws the UI
  and plays video with FFmpeg: H.264 240p is decoded in software straight to the panel's RGB565
  size, with AAC audio through PulseAudio as the clock. The player takes two
  inputs, because YouTube serves picture and sound as separate streams.
- **The helper** (`/usr/lib/zerotube/zerotube-helper.py`) is one long-lived Python process that
  imports [yt-dlp](https://github.com/yt-dlp/yt-dlp) once and answers JSON lines on
  stdin/stdout: search, feeds, account, resolving a video, thumbnails (into tmpfs, not the SD
  card), and the sign-in page.
- **A loopback proxy** in the helper feeds FFmpeg. googlevideo throttles a stream fetched in
  one piece to about real time; the proxy turns FFmpeg's reads and seeks into 1 MB range
  requests, which arrive at full speed.
- **yt-dlp lives in a venv** at `~/.local/share/zerotube/venv`, installed with pip on first
  launch and updated with `U`, because YouTube breaks old yt-dlp releases within weeks and
  Debian's is always behind.
- Videos are resolved **without a JavaScript runtime** on purpose: with one, yt-dlp switches to
  web clients whose stream URLs now need a PO token and return 403 (seen 2026-11). Only if that
  fails and you are signed in does it retry with cookies and quickjs.

Measured on a CardputerZero (CM0) over Wi-Fi: helper up 2.8 s after launch, search 3 s, a video
resolved in 2.8 s, 240p decoding at 10x real time on three cores; helper RSS 63 MB.

## Build

Simulator (macOS, Homebrew `cmake ninja sdl2 freetype libpng jpeg-turbo fmt ffmpeg`):

```sh
cmake --preset darwin-arm64 && cmake --build --preset darwin-arm64-dbg
ZEROTUBE_PYTHON=/path/to/venv/with/yt-dlp/bin/python ./build/darwin-arm64/Debug/zerotube
```

`ZEROTUBE_SCRIPT="2500:ENTER;1500:s;200:c;...;300:shot;300:quit"` drives the simulator for
testing (keys, `shot` screenshots into `~/Pictures/Screenshots`).

Device package: build natively on an arm64 Debian trixie host, in a Docker image that has the
CardputerZero build dependencies plus the FFmpeg and PulseAudio dev packages.
`HOST=user@build-host IMAGE=<your image> tools/build-device.sh` syncs the source there, builds,
and copies `dist/ZeroTube_<version>_m5stack1_arm64.deb` back.

## License

MIT, see LICENSE and NOTICE. Fonts are under their own licenses (assets/fonts/LICENSE.txt).
