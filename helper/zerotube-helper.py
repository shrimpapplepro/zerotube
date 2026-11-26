#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ZeroTube contributors
# SPDX-License-Identifier: MIT
"""ZeroTube's YouTube side: one long-lived process the app talks to over
stdin/stdout, one JSON object per line.

  app -> helper   {"id": 7, "cmd": "search", "q": "lofi"}
  helper -> app   {"id": 7, "ok": true, "items": [...]}      (the reply)
                  {"ev": "thumb", "vid": "...", "path": "..."} (events, no id)

Everything YouTube is done by yt-dlp, imported once so a request does not pay
the 3-5 s interpreter + import start-up of the yt-dlp command on the CM0.
Each request runs on its own thread, so a slow feed never holds up a search or
the thumbnails.

Playback goes through a loopback proxy (127.0.0.1, random port). googlevideo
throttles a stream to ~real time unless it is fetched in byte ranges of a few
MB, which FFmpeg's http reader does not do; the proxy turns FFmpeg's one long
GET (and its seeks) into 1 MB range requests on a kept-alive connection.
"""

import http.client
import http.server
import json
import os
import re
import secrets
import socket
import socketserver
import sys
import threading
import time
import urllib.parse
import urllib.request

APP = "zerotube"
HOME = os.path.expanduser("~")
CONFIG_DIR = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.join(HOME, ".config"), APP)
COOKIES = os.path.join(CONFIG_DIR, "cookies.txt")
# Thumbnails are throwaway: keep them in RAM (tmpfs), not on the SD card.
RUNTIME = os.environ.get("XDG_RUNTIME_DIR") or "/tmp"
THUMBS = os.path.join(RUNTIME, APP, "thumbs")
SIGNIN_PORT = int(os.environ.get("ZEROTUBE_SIGNIN_PORT", "8791"))
CHUNK = 1 << 20  # googlevideo serves ranges this size at full speed

FEEDS = {
    "home": ":ytrec",
    "subs": ":ytsubs",
    "history": ":ythis",
    "watchlater": ":ytwatchlater",
    "liked": ":ytfav",
}

_out_lock = threading.Lock()


def send(obj):
    line = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
    with _out_lock:
        sys.stdout.write(line + "\n")
        sys.stdout.flush()


def log(*args):
    print("[helper]", *args, file=sys.stderr, flush=True)


try:
    import yt_dlp  # noqa: E402
    from yt_dlp.version import __version__ as YTDLP_VERSION
except Exception as exc:  # the engine is not installed yet
    yt_dlp = None
    YTDLP_VERSION = ""
    IMPORT_ERROR = str(exc)
else:
    IMPORT_ERROR = ""


def signed_in():
    try:
        with open(COOKIES, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return False
    return bool(re.search(r"youtube\.com\t.*\t(SAPISID|__Secure-3PAPISID)\t", text))


class Quiet:
    def debug(self, msg):
        pass

    def info(self, msg):
        pass

    def warning(self, msg):
        log("warn:", msg[:300])

    def error(self, msg):
        log("error:", msg[:300])


def ydl(cookies=True, **extra):
    opts = {
        "quiet": True,
        "no_warnings": False,
        "logger": Quiet(),
        "skip_download": True,
        "noplaylist": True,
        "socket_timeout": 20,
        "extractor_retries": 1,
    }
    if cookies and signed_in():
        opts["cookiefile"] = COOKIES
    opts.update(extra)
    return yt_dlp.YoutubeDL(opts)


# ---- lists -----------------------------------------------------------------

def thumb_url(vid):
    # 120x90 (4:3, letterboxed): ~4 KB, plenty for a 64x36 tile on slow Wi-Fi.
    return "https://i.ytimg.com/vi/%s/default.jpg" % vid


def entry_item(e):
    vid = e.get("id")
    if not vid or len(vid) != 11 or e.get("ie_key") not in (None, "Youtube"):
        return None
    url = e.get("url") or ""
    if "/shorts/" in url:
        kind = "short"
    else:
        kind = "video"
    live = e.get("live_status") == "is_live"
    return {
        "id": vid,
        "title": e.get("title") or "",
        "channel": e.get("channel") or e.get("uploader") or "",
        "duration": int(e.get("duration") or 0),
        "views": int(e.get("view_count") or 0),
        "live": live,
        "kind": kind,
    }


def flat_list(url, limit):
    with ydl(extract_flat="in_playlist", playlistend=limit, noplaylist=False) as y:
        info = y.extract_info(url, download=False, process=True)
    items = []
    seen = set()
    for e in info.get("entries") or []:
        if not e:
            continue
        it = entry_item(e)
        if it and it["id"] not in seen:
            seen.add(it["id"])
            items.append(it)
    return items


def cmd_search(req):
    q = (req.get("q") or "").strip()
    if not q:
        return {"items": []}
    return {"items": flat_list("ytsearch%d:%s" % (int(req.get("limit") or 20), q), 40)}


def cmd_feed(req):
    name = req.get("feed") or "home"
    url = FEEDS.get(name)
    if not url:
        raise ValueError("unknown feed " + name)
    if name != "home" and not signed_in():
        return {"items": [], "needs_signin": True}
    items = flat_list(url, int(req.get("limit") or 30))
    out = {"items": items}
    if name == "home" and not items and not signed_in():
        out["needs_signin"] = True
    return out


# ---- thumbnails ------------------------------------------------------------

_thumb_sem = threading.Semaphore(4)


def fetch_thumb(vid, url):
    path = os.path.join(THUMBS, vid + ".jpg")
    if os.path.exists(path):
        send({"ev": "thumb", "vid": vid, "path": path})
        return
    with _thumb_sem:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=15) as r:
                data = r.read()
            tmp = path + ".part"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, path)
        except Exception as exc:
            log("thumb", vid, exc)
            return
    send({"ev": "thumb", "vid": vid, "path": path})


def cmd_thumbs(req):
    os.makedirs(THUMBS, exist_ok=True)
    for vid in req.get("ids") or []:
        if isinstance(vid, str) and re.fullmatch(r"[\w-]{11}", vid):
            threading.Thread(target=fetch_thumb, args=(vid, thumb_url(vid)), daemon=True).start()
    return {}


# ---- playback proxy --------------------------------------------------------

class Stream:
    def __init__(self, url, headers, size):
        self.url = url
        self.headers = {k: v for k, v in (headers or {}).items()}
        self.size = size or 0


_streams = {}
_streams_lock = threading.Lock()


def upstream_size(stream):
    if stream.size:
        return stream.size
    resp, conn = range_get(stream, 0, 0)
    cr = resp.getheader("Content-Range") or ""
    resp.read()
    conn.close()
    m = re.search(r"/(\d+)$", cr)
    stream.size = int(m.group(1)) if m else 0
    return stream.size


def range_get(stream, start, end, conn=None, redirects=5):
    url = stream.url
    for _ in range(redirects):
        u = urllib.parse.urlsplit(url)
        if conn is None or getattr(conn, "_zt_host", None) != u.netloc:
            if conn is not None:
                conn.close()
            conn = http.client.HTTPSConnection(u.netloc, timeout=20)
            conn._zt_host = u.netloc
        path = u.path + ("?" + u.query if u.query else "")
        hdrs = dict(stream.headers)
        hdrs["Range"] = "bytes=%d-%d" % (start, end)
        hdrs["Accept-Encoding"] = "identity"
        conn.request("GET", path, headers=hdrs)
        resp = conn.getresponse()
        if resp.status in (301, 302, 303, 307, 308):
            url = urllib.parse.urljoin(url, resp.getheader("Location"))
            resp.read()
            continue
        if resp.status not in (200, 206):
            body = resp.read(200)
            raise IOError("upstream %d %r" % (resp.status, body[:80]))
        return resp, conn
    raise IOError("too many redirects")


class ProxyHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_HEAD(self):
        self.handle_get(head=True)

    def do_GET(self):
        self.handle_get(head=False)

    def handle_get(self, head):
        token = self.path.rsplit("/", 1)[-1]
        with _streams_lock:
            stream = _streams.get(token)
        if not stream:
            self.send_error(404)
            return
        try:
            size = upstream_size(stream)
        except Exception as exc:
            log("proxy size", exc)
            self.send_error(502)
            return
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        partial = False
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng)
            if m:
                if m.group(1):
                    start = int(m.group(1))
                    if m.group(2):
                        end = min(int(m.group(2)), size - 1)
                elif m.group(2):  # suffix range
                    start = max(0, size - int(m.group(2)))
                partial = True
        if start >= size:
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % size)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if partial:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        if head:
            return
        conn = None
        pos = start
        try:
            while pos <= end:
                chunk_end = min(end, pos + CHUNK - 1)
                for attempt in range(3):
                    try:
                        resp, conn = range_get(stream, pos, chunk_end, conn)
                        while True:
                            data = resp.read(64 * 1024)
                            if not data:
                                break
                            self.wfile.write(data)
                            pos += len(data)
                        break
                    except (BrokenPipeError, ConnectionResetError):
                        raise
                    except Exception as exc:
                        log("proxy chunk", pos, exc)
                        if conn is not None:
                            conn.close()
                            conn = None
                        if attempt == 2:
                            raise
                        time.sleep(0.5)
                if pos <= chunk_end:  # short read: retry from where it stopped
                    continue
        except (BrokenPipeError, ConnectionResetError):
            pass  # FFmpeg seeked or closed
        except Exception as exc:
            log("proxy", exc)
            self.close_connection = True
        finally:
            if conn is not None:
                conn.close()


class ThreadingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


_proxy = ThreadingServer(("127.0.0.1", 0), ProxyHandler)
threading.Thread(target=_proxy.serve_forever, daemon=True).start()
PROXY_BASE = "http://127.0.0.1:%d/s/" % _proxy.server_address[1]

# Small and cheap to decode: H.264 at the panel's size first, then anything.
FORMAT = (
    "bv*[vcodec^=avc1][height<=240]+ba[ext=m4a]/"
    "bv*[vcodec^=avc1][height<=360]+ba[ext=m4a]/"
    "bv*[height<=360]+ba/b[height<=360]/bv*+ba/b"
)


def register(fmt):
    if (fmt.get("protocol") or "").startswith("m3u8"):
        # Live (HLS): short segments, no throttling, and FFmpeg reads the
        # playlist itself. The proxy only helps one long file.
        return fmt["url"]
    token = secrets.token_urlsafe(9)
    size = fmt.get("filesize") or 0
    with _streams_lock:
        _streams[token] = Stream(fmt["url"], fmt.get("http_headers"), size)
    return PROXY_BASE + token


def resolve(vid, cookies):
    # No JS runtime on the first try, on purpose: with one, yt-dlp switches to
    # web clients whose stream URLs now need a PO token and answer 403, while
    # the client it uses without one streams fine (2026-11). The signed-in
    # retry may use quickjs/deno for the challenge.
    runtimes = {"deno": {"path": None}, "quickjs": {"path": None}} if cookies else {}
    with ydl(cookies=cookies, format=FORMAT, js_runtimes=runtimes) as y:
        return y.extract_info("https://www.youtube.com/watch?v=" + vid, download=False)


def cmd_resolve(req):
    vid = req.get("vid") or ""
    if not re.fullmatch(r"[\w-]{11}", vid):
        raise ValueError("bad video id")
    # Signed out first: today that path needs no JavaScript challenge solving,
    # which the CM0 would spend a minute on. Signed in (members-only,
    # age-gated) only when that fails.
    try:
        info = resolve(vid, cookies=False)
    except Exception as first:
        if not signed_in():
            raise
        log("anonymous resolve failed, retrying signed in:", first)
        info = resolve(vid, cookies=True)
    with _streams_lock:
        _streams.clear()  # one video at a time
    fmts = info.get("requested_formats") or [info]
    video = audio = ""
    for f in fmts:
        if not f.get("url"):
            continue
        if f.get("vcodec") not in (None, "none"):
            if not video:
                video = register(f)
            if f.get("acodec") not in (None, "none") and not audio:
                audio = ""  # muxed: the one URL carries both
        elif f.get("acodec") not in (None, "none") and not audio:
            audio = register(f)
    if not video and not audio:
        raise ValueError("no playable stream")
    if not video:
        video, audio = audio, ""
    return {
        "video": video,
        "audio": audio,
        "title": info.get("title") or "",
        "channel": info.get("channel") or info.get("uploader") or "",
        "duration": int(info.get("duration") or 0),
        "live": bool(info.get("is_live")),
    }


# ---- account ---------------------------------------------------------------

def find_key(obj, key):
    if isinstance(obj, dict):
        if key in obj:
            return obj[key]
        for v in obj.values():
            r = find_key(v, key)
            if r is not None:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = find_key(v, key)
            if r is not None:
                return r
    return None


def text_of(node):
    if not isinstance(node, dict):
        return ""
    if "simpleText" in node:
        return node["simpleText"]
    return "".join(r.get("text", "") for r in node.get("runs") or [])


def cmd_account(req):
    out = {"signed_in": signed_in(), "engine": YTDLP_VERSION}
    if not out["signed_in"]:
        return out
    try:
        with ydl() as y:
            ie = y.get_info_extractor("YoutubeTab")
            ytcfg = ie._download_ytcfg("web", None) or {}
            data = ie._extract_response(
                item_id="account", query={}, ep="account/account_menu", ytcfg=ytcfg,
                headers=ie.generate_api_headers(ytcfg=ytcfg), fatal=False)
        head = find_key(data, "activeAccountHeaderRenderer") or {}
        out["name"] = text_of(head.get("accountName"))
        out["handle"] = text_of(head.get("channelHandle"))
        thumbs = (head.get("accountPhoto") or {}).get("thumbnails") or []
        if thumbs:
            url = thumbs[0]["url"]
            if url.startswith("//"):
                url = "https:" + url
            url = re.sub(r"=s\d+", "=s88", url)
            os.makedirs(THUMBS, exist_ok=True)
            path = os.path.join(THUMBS, "avatar.jpg")
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"}), timeout=15) as r:
                data = r.read()
            with open(path, "wb") as f:
                f.write(data)
            out["avatar"] = path
        if not out["name"]:
            out["expired"] = True  # cookies there, but YouTube no longer accepts them
    except Exception as exc:
        log("account", exc)
        out["error"] = str(exc)[:200]
    return out


def cmd_signout(req):
    try:
        os.remove(COOKIES)
    except FileNotFoundError:
        pass
    return {"signed_in": False}


# ---- sign-in page ------------------------------------------------------------

SIGNIN_PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ZeroTube sign-in</title><style>
body{font:16px system-ui,sans-serif;background:#0f0f0f;color:#f1f1f1;margin:0;padding:20px;max-width:560px}
h1{font-size:20px}b{color:#fff}ol{padding-left:20px;color:#aaa}li{margin:6px 0}
textarea{width:100%;height:140px;background:#121212;color:#f1f1f1;border:1px solid #303030;border-radius:8px;font:12px monospace}
button{background:#f1f1f1;color:#0f0f0f;border:0;border-radius:18px;padding:10px 18px;font-weight:600;margin-top:10px}
.ok{color:#3ea6ff}.err{color:#ff4e45}</style></head><body>
<h1>Sign in ZeroTube on your CardputerZero</h1>
<ol><li>In a <b>private window</b> on your computer, sign in to youtube.com.</li>
<li>Export the cookies for youtube.com in <b>Netscape cookies.txt</b> format (for example with the
"Get cookies.txt LOCALLY" extension), then close the private window so YouTube does not rotate them.</li>
<li>Choose the file or paste its text below.</li></ol>
<form method="post" enctype="multipart/form-data">
<input type="file" name="file" accept=".txt,text/plain"><br><br>
<textarea name="text" placeholder="# Netscape HTTP Cookie File ..."></textarea><br>
<button>Send to device</button></form>{note}</body></html>"""


def parse_multipart(body, ctype):
    m = re.search(r'boundary="?([^";]+)"?', ctype or "")
    if not m:
        return {}
    boundary = ("--" + m.group(1)).encode()
    fields = {}
    for part in body.split(boundary):
        head, _, data = part.partition(b"\r\n\r\n")
        nm = re.search(rb'name="([^"]+)"', head)
        if not nm:
            continue
        fields[nm.group(1).decode()] = data.rstrip(b"\r\n-")
    return fields


def clean_cookies(text):
    """Keeps only the YouTube/Google lines of a Netscape cookie file."""
    lines = ["# Netscape HTTP Cookie File"]
    for line in text.splitlines():
        line = line.strip("\r")
        if line.startswith("#HttpOnly_"):
            probe = line[len("#HttpOnly_"):]
        elif line.startswith("#") or not line.strip():
            continue
        else:
            probe = line
        f = probe.split("\t")
        if len(f) != 7:
            continue
        if f[0].lstrip(".").endswith(("youtube.com", "google.com")):
            lines.append(line)
    return "\n".join(lines) + "\n"


_signin_lock = threading.Lock()
_signin_server = None


class SigninHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def page(self, note, status=200):
        body = SIGNIN_PAGE.replace("{note}", note).encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        log("signin page: GET from", self.client_address[0])
        self.page("")

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        log("signin page: POST from", self.client_address[0], "bytes", length)
        if length > 2 * 1024 * 1024:
            self.page('<p class="err">That file is too big for a cookie file.</p>', 413)
            return
        fields = parse_multipart(self.rfile.read(length), self.headers.get("Content-Type"))
        raw = (fields.get("file") or b"").strip() or (fields.get("text") or b"").strip()
        text = clean_cookies(raw.decode("utf-8", "replace"))
        if not re.search(r"youtube\.com\t.*\t(SAPISID|__Secure-3PAPISID)\t", text):
            log("signin page: rejected, no SAPISID (%d cookie lines kept)" % (text.count("\n") - 1))
            self.page('<p class="err">No signed-in YouTube cookies found (SAPISID). '
                      'Export them while signed in to youtube.com.</p>')
            return
        os.makedirs(CONFIG_DIR, exist_ok=True)
        tmp = COOKIES + ".part"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.replace(tmp, COOKIES)
        log("signin page: saved %d cookie lines" % (text.count("\n") - 1))
        self.page('<p class="ok">Signed in. You can close this page.</p>')
        send({"ev": "signed_in"})
        threading.Thread(target=stop_signin, daemon=True).start()


def lan_addresses():
    addrs = []
    try:
        import subprocess
        out = subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=3).stdout
        addrs = [a for a in out.split() if ":" not in a]
    except Exception:
        pass
    if not addrs:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("192.0.2.1", 9))
            addrs = [s.getsockname()[0]]
            s.close()
        except OSError:
            pass
    # A home LAN address first; Tailscale (100.64/10) as the second choice.
    addrs.sort(key=lambda a: a.startswith("100."))
    return addrs


def cmd_signin_start(req):
    global _signin_server
    with _signin_lock:
        if _signin_server is None:
            srv = ThreadingServer(("0.0.0.0", SIGNIN_PORT), SigninHandler)
            threading.Thread(target=srv.serve_forever, daemon=True).start()
            _signin_server = srv
    addrs = lan_addresses()
    return {"urls": ["http://%s:%d/" % (a, SIGNIN_PORT) for a in addrs]}


def stop_signin(req=None):
    global _signin_server
    log("signin page: stopping (%s)" % ("app asked" if req is not None else "after a good upload"))
    with _signin_lock:
        srv, _signin_server = _signin_server, None
    if srv:
        srv.shutdown()
        srv.server_close()
    return {}


# ---- engine ---------------------------------------------------------------

def cmd_hello(req):
    return {"engine": YTDLP_VERSION, "import_error": IMPORT_ERROR, "signed_in": signed_in()}


HANDLERS = {
    "hello": cmd_hello,
    "search": cmd_search,
    "feed": cmd_feed,
    "thumbs": cmd_thumbs,
    "resolve": cmd_resolve,
    "account": cmd_account,
    "signout": cmd_signout,
    "signin_start": cmd_signin_start,
    "signin_stop": stop_signin,
}

NEEDS_ENGINE = {"search", "feed", "resolve", "account"}


def short_error(exc):
    msg = str(exc)
    msg = re.sub(r"^ERROR:\s*", "", msg)
    msg = re.sub(r"\[[\w:]+\]\s*[\w-]{11}:\s*", "", msg)
    msg = msg.split("\n")[0]
    if "Sign in to confirm" in msg:
        return "YouTube wants a sign-in for this video"
    if "Private video" in msg:
        return "This video is private"
    if "members" in msg.lower():
        return "Members-only video"
    return msg[:160]


def run(req):
    rid = req.get("id")
    cmd = req.get("cmd")
    handler = HANDLERS.get(cmd)
    try:
        if handler is None:
            raise ValueError("unknown command %r" % cmd)
        if cmd in NEEDS_ENGINE and yt_dlp is None:
            raise RuntimeError("YouTube engine not installed")
        result = handler(req) or {}
        result.update({"id": rid, "ok": True})
        send(result)
    except Exception as exc:
        log(cmd, "failed:", repr(exc)[:300])
        send({"id": rid, "ok": False, "error": short_error(exc)})


def main():
    send({"ev": "ready", "engine": YTDLP_VERSION, "import_error": IMPORT_ERROR, "signed_in": signed_in()})
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        threading.Thread(target=run, args=(req,), daemon=True).start()


if __name__ == "__main__":
    main()
