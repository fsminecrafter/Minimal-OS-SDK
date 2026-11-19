#!/usr/bin/env python3
"""
dlrd - Deliver server for Minimal-OS, Linux/Python edition.

One program, three faces:

  * the native dlr protocol (default TCP 7870). The Minimal-OS `dlr` client
    and Deliver's C++ client work against it unchanged.
  * a public website (default TCP 7871, optionally HTTPS on 7872):
        /                       home page
        /list                   every package, its files and details
        /api/package            JSON package list   (?q=text searches)
        /api/package/<name>     the archive itself  (what `dlr install` fetches)
        /api/package/<name>/info  JSON details for one package
  * a local admin window (default http://127.0.0.1:8081): drop folders in to
    publish packages, a Settings button for name/password/ports, file listing.

Multiple cores: the master process binds the sockets, then forks one worker per
core; every worker accepts on the same sockets. State lives on disk, so workers
need no coordination - the admin window just changes files.

Wire format notes live in programs/dlr/dlr_proto.h; this file follows them.
Needs: Python 3.8+, `cryptography` (AES-256-GCM).
"""
import argparse
import base64
import binascii
import hashlib
import hmac
import html
import json
import multiprocessing
import os
import re
import secrets
import select
import shutil
import signal
import socket
import ssl
import struct
import sys
import tarfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from cryptography.exceptions import InvalidTag
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:                                    # pragma: no cover
    sys.exit("dlrd needs the 'cryptography' package:  pip install cryptography")

# ---- wire constants (mirror programs/dlr/dlr.h) -----------------------------
MSG_HELLO_ACK, MSG_AUTH_REQUEST = 0x02, 0x03
MSG_INSTALL_REQUEST, MSG_INSTALL_DATA, MSG_INSTALL_END, MSG_INSTALL_ERROR = 0x10, 0x11, 0x12, 0x13
MSG_SEARCH_REQUEST, MSG_SEARCH_RESULT, MSG_PKG_LIST = 0x20, 0x21, 0x22
MSG_PING, MSG_PONG, MSG_ERROR = 0x30, 0x31, 0xFF
DISCOVERY_PORT = 4243
PROTOCOL_VERSION = 1
# The Minimal-OS client refuses frames over 8 KiB, so a chunk plus type byte
# plus 28 bytes of GCM overhead has to fit. 4096 is what the Minimal-OS server
# sends too.
MAX_CHUNK = 4096
LIST_MAX = 8 * 1024 - 256          # keep PKG_LIST inside that same frame limit

NAME_RE = re.compile(r"^[A-Za-z0-9_-][A-Za-z0-9._-]{0,62}$")
LOOPBACK_HOSTS = {"localhost", "127.0.0.1", "[::1]", "::1"}

DEFAULTS = {
    "name": "minimalos",
    "password_hash": "",           # sha256 hex of the dlr password ("" = open)
    "dlr_port": 7870,              # native protocol, 0 = off
    "http_port": 7871,            # website + /api/package, 0 = off
    "udp_port": 7870,              # dlr over UDP (same number as the TCP port is fine), 0 = off
    "udp_public": "",              # host:port people reach the UDP side at (e.g. the playit.gg address)
    "https_port": 7872,            # needs cert_file + key_file, 0 = off
    "cert_file": "",
    "key_file": "",
    "sniff_dlr_on_http": False,    # also answer dlr clients on the web ports
    "public_host": "",             # name shown in install commands (default: the Host header)
    "workers": 0,                  # 0 = one per CPU core
    "admin_host": "127.0.0.1",
    "admin_port": 8081,            # 0 = no admin window
    "admin_password": "",          # pbkdf2$salt$hash; required off loopback
    "broadcast": False,            # LAN discovery hello on UDP 4243
    "chunk_size": MAX_CHUNK,
    "max_upload_mb": 1024,
    "max_connections": 128,        # per worker
}


# =============================================================================
# configuration
# =============================================================================
def sha256_hex(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def hash_admin_password(pw):
    salt = secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), 200_000).hex()
    return "pbkdf2$%s$%s" % (salt, digest)


def check_admin_password(stored, pw):
    try:
        _, salt, want = stored.split("$")
        got = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(salt), 200_000).hex()
        return hmac.compare_digest(got, want)
    except ValueError:
        return False


def clean_name(text):
    """Server name goes into '|'-separated frames, so strip what would break them."""
    text = re.sub(r"[|\r\n\t]", " ", str(text)).strip()
    return text[:63] or "minimalos"


def normalise(raw):
    cfg = dict(DEFAULTS)
    cfg.update({k: v for k, v in raw.items() if k in DEFAULTS})
    cfg["name"] = clean_name(cfg["name"])
    for key, lo, hi in (("dlr_port", 0, 65535), ("http_port", 0, 65535), ("https_port", 0, 65535), ("udp_port", 0, 65535),
                        ("admin_port", 0, 65535), ("workers", 0, 256), ("max_upload_mb", 1, 1 << 20),
                        ("max_connections", 4, 100000), ("chunk_size", 512, 60000)):
        try:
            cfg[key] = min(hi, max(lo, int(cfg[key])))
        except (TypeError, ValueError):
            cfg[key] = DEFAULTS[key]
    for key in ("sniff_dlr_on_http", "broadcast"):
        cfg[key] = bool(cfg[key])
    for key in ("cert_file", "key_file", "admin_host", "password_hash", "admin_password", "public_host", "udp_public"):
        cfg[key] = str(cfg[key] or "")
    if cfg["password_hash"] and not re.fullmatch(r"[0-9a-f]{64}", cfg["password_hash"]):
        cfg["password_hash"] = ""
    return cfg


class Config:
    """JSON file, re-read when it changes, so every worker sees admin edits."""

    def __init__(self, path):
        self.path = path
        self._lock = threading.RLock()          # update() calls get() while holding it
        self._mtime = "unset"
        self._data = dict(DEFAULTS)

    def get(self):
        try:
            mtime = os.stat(self.path).st_mtime_ns
        except FileNotFoundError:
            mtime = None
        if mtime != self._mtime:
            with self._lock:
                try:
                    with open(self.path, encoding="utf-8") as f:
                        self._data = normalise(json.load(f))
                except (OSError, ValueError):
                    self._data = normalise({})
                self._mtime = mtime
        return self._data

    def update(self, changes):
        with self._lock:
            data = dict(self.get())
            data.update(changes)
            data = normalise(data)
            tmp = self.path + ".tmp"
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            os.replace(tmp, self.path)
            self._data = data
            self._mtime = os.stat(self.path).st_mtime_ns
            return data


# =============================================================================
# package store
# =============================================================================
def valid_rel_path(rel):
    """A path inside a package: relative, forward slashes, nothing hidden or odd."""
    if not rel or len(rel) > 400 or rel.startswith("/") or "\\" in rel or ":" in rel or "\0" in rel:
        return False
    parts = rel.split("/")
    return all(p and p not in (".", "..") and not p.startswith(".") for p in parts)


def parse_manifest(text):
    """Minimal .pkg reader: [Info] keys, case-insensitive, like dlr_pkg.c."""
    info, section = {}, ""
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in "#;":
            continue
        if line[0] == "[" and "]" in line:
            section = line[1:line.index("]")].strip().lower()
        elif "=" in line and section == "info":
            k, v = line.split("=", 1)
            info[k.strip().lower()] = v.strip()
    return info


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return ("%d %s" % (n, unit)) if unit == "B" else ("%.1f %s" % (n, unit))
        n /= 1024.0


class Store:
    """
        packages/<name>/       the package's files (incl. its .pkg manifest)
        cache/<name>.tar       the archive clients download (built once)
        cache/<name>.json      its details: version, size, sha256, file list
        staging/               uploads in progress

    A package is "published" when its cache entry exists. sync() keeps the
    cache in step with packages/, so dropping a folder into packages/ by hand
    works exactly like dragging it into the window.
    """

    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.packages = os.path.join(self.root, "packages")
        self.cache = os.path.join(self.root, "cache")
        self.staging = os.path.join(self.root, "staging")
        for d in (self.packages, self.cache, self.staging):
            os.makedirs(d, exist_ok=True)
        self._build_lock = threading.Lock()

    # -- paths
    def pkg_dir(self, name):   return os.path.join(self.packages, name)
    def tar_path(self, name):  return os.path.join(self.cache, name + ".tar")
    def meta_path(self, name): return os.path.join(self.cache, name + ".json")

    # -- reading (used by every worker, cheap)
    def get(self, name):
        if not NAME_RE.match(name or ""):
            return None
        try:
            with open(self.meta_path(name), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def list(self):
        out = []
        try:
            names = sorted(e[:-5] for e in os.listdir(self.cache) if e.endswith(".json"))
        except OSError:
            return out
        for name in names:
            meta = self.get(name)
            if meta and os.path.exists(self.tar_path(name)):
                out.append(meta)
        return out

    def search(self, query):
        q = (query or "").lower()
        return [m for m in self.list()
                if not q or q in m["name"].lower() or q in m["description"].lower()]

    # -- building
    @staticmethod
    def _walk(root):
        """(relpath, abspath, is_dir) for everything publishable, sorted."""
        entries = []
        for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
            dirnames[:] = sorted(d for d in dirnames
                                 if not d.startswith(".") and not os.path.islink(os.path.join(dirpath, d)))
            rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
            for d in dirnames:
                entries.append((d if rel_dir == "." else rel_dir + "/" + d, os.path.join(dirpath, d), True))
            for f in sorted(filenames):
                ap = os.path.join(dirpath, f)
                if f.startswith(".") or os.path.islink(ap) or not os.path.isfile(ap):
                    continue
                entries.append((f if rel_dir == "." else rel_dir + "/" + f, ap, False))
        return entries

    def signature(self, name):
        count, total, newest = 0, 0, 0
        for rel, ap, is_dir in self._walk(self.pkg_dir(name)):
            st = os.stat(ap)
            count += 1
            total += 0 if is_dir else st.st_size
            newest = max(newest, st.st_mtime_ns)
        return [count, total, newest]

    def build(self, name):
        """(Re)build the archive + details for packages/<name>. Returns the meta."""
        if not NAME_RE.match(name):
            raise ValueError("invalid package name")
        root = self.pkg_dir(name)
        if not os.path.isdir(root):
            raise FileNotFoundError(name)
        with self._build_lock:
            manifests = sorted(e for e in os.listdir(root)
                               if e.endswith(".pkg") and os.path.isfile(os.path.join(root, e)))
            if not manifests:                      # a package needs a manifest to be installable
                with open(os.path.join(root, name + ".pkg"), "w", encoding="utf-8") as f:
                    f.write("[Info]\nname=%s\nversion=1.0\ndescription=\narch=x86_64\n"
                            "operatingsystem=MINIMALOS\n" % name)
                manifests = [name + ".pkg"]
            with open(os.path.join(root, manifests[0]), encoding="utf-8", errors="replace") as f:
                info = parse_manifest(f.read())

            entries = self._walk(root)
            tmp = self.tar_path(name) + ".tmp"
            files, total = [], 0
            # Entries are prefixed "<name>/", as the C++ server does; the client
            # strips exactly that prefix (see canonical_rel in dlr_client.c).
            with tarfile.open(tmp, "w", format=tarfile.GNU_FORMAT) as tf:
                top = tarfile.TarInfo(name)
                top.type, top.mode, top.mtime = tarfile.DIRTYPE, 0o755, int(time.time())
                tf.addfile(top)
                for rel, ap, is_dir in entries:
                    st = os.stat(ap)
                    ti = tarfile.TarInfo(name + "/" + rel)
                    ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = int(st.st_mtime), 0, 0, "", ""
                    if is_dir:
                        ti.type, ti.mode = tarfile.DIRTYPE, 0o755
                        tf.addfile(ti)
                    else:
                        ti.size, ti.mode = st.st_size, (0o755 if st.st_mode & 0o111 else 0o644)
                        with open(ap, "rb") as fh:
                            tf.addfile(ti, fh)
                        files.append({"path": rel, "size": st.st_size})
                        total += st.st_size
            h = hashlib.sha256()
            with open(tmp, "rb") as f:
                for block in iter(lambda: f.read(1 << 20), b""):
                    h.update(block)
            size = os.path.getsize(tmp)
            os.replace(tmp, self.tar_path(name))

            meta = {
                "name": name,
                "manifest_name": info.get("name", name),
                "version": info.get("version", ""),
                "description": info.get("description", ""),
                "arch": info.get("arch", ""),
                "os": info.get("operatingsystem", ""),
                "format": "tar",
                "size": size,
                "sha256": h.hexdigest(),
                "file_count": len(files),
                "total_bytes": total,
                "files": files,
                "signature": self.signature(name),
                "built": int(time.time()),
            }
            mtmp = self.meta_path(name) + ".tmp"
            with open(mtmp, "w", encoding="utf-8") as f:
                json.dump(meta, f)
            os.replace(mtmp, self.meta_path(name))
            return meta

    def sync(self):
        """Publish new/changed folders under packages/, drop vanished ones."""
        try:
            dirs = {e for e in os.listdir(self.packages)
                    if NAME_RE.match(e) and os.path.isdir(self.pkg_dir(e))}
        except OSError:
            return
        for name in sorted(dirs):
            try:
                meta = self.get(name)
                if (meta is None or not os.path.exists(self.tar_path(name))
                        or meta.get("signature") != self.signature(name)):
                    self.build(name)
                    log("published %s" % name)
            except (OSError, ValueError, tarfile.TarError) as e:
                log("cannot publish %s: %s" % (name, e))
        for entry in os.listdir(self.cache):
            base = entry.rsplit(".", 1)[0]
            if entry.endswith((".tar", ".json")) and base not in dirs:
                try:
                    os.remove(os.path.join(self.cache, entry))
                except OSError:
                    pass

    def remove(self, name):
        if not NAME_RE.match(name):
            return False
        existed = os.path.isdir(self.pkg_dir(name))
        shutil.rmtree(self.pkg_dir(name), ignore_errors=True)
        for p in (self.tar_path(name), self.meta_path(name)):
            try:
                os.remove(p)
            except OSError:
                pass
        return existed


# =============================================================================
# logging, small helpers
# =============================================================================
_log_lock = threading.Lock()


def log(msg):
    with _log_lock:
        print("%s [%d] %s" % (time.strftime("%H:%M:%S"), os.getpid(), msg), flush=True)


def peer_str(addr):
    return addr[0] if addr else "?"


class AuthLimiter:
    """Per-process brake on password guessing: 10 failures a minute per address."""

    def __init__(self):
        self._lock = threading.Lock()
        self._fails = {}

    def blocked(self, ip):
        with self._lock:
            now = time.time()
            recent = [t for t in self._fails.get(ip, []) if now - t < 60]
            self._fails[ip] = recent
            return len(recent) >= 10

    def fail(self, ip):
        with self._lock:
            self._fails.setdefault(ip, []).append(time.time())
            if len(self._fails) > 5000:
                self._fails.clear()


LIMITER = AuthLimiter()


def password_ok(cfg, supplied):
    return hmac.compare_digest(sha256_hex(supplied), cfg["password_hash"])


# =============================================================================
# the native dlr protocol
# =============================================================================
def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return bytes(buf)


def frame_send(sock, payload):
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def frame_recv(sock, cap=1 << 16):
    (n,) = struct.unpack(">I", recv_exact(sock, 4))
    if n == 0 or n > cap:
        raise ConnectionError("bad frame length %d" % n)
    return recv_exact(sock, n)


class DlrSession:
    def __init__(self, sock, aes):
        self.sock, self.aes = sock, aes

    def seal(self, mtype, body=b""):
        nonce = os.urandom(12)
        return nonce + self.aes.encrypt(nonce, bytes([mtype]) + body, None)

    def send(self, mtype, body=b""):
        frame_send(self.sock, self.seal(mtype, body))

    def recv(self):
        frame = frame_recv(self.sock)
        if len(frame) < 12 + 16 + 1:
            raise ConnectionError("short frame")
        try:
            plain = self.aes.decrypt(frame[:12], frame[12:], None)
        except InvalidTag:
            raise ConnectionError("bad tag")
        return plain[0], plain[1:]


def list_lines(metas):
    """name|version|description\\n, as many whole lines as fit in one frame."""
    out = bytearray()
    for m in metas:
        clean = lambda s: re.sub(r"[|\r\n\t]", " ", s)
        line = ("%s|%s|%s\n" % (m["name"], clean(m["version"]), clean(m["description"]))).encode()
        if len(out) + len(line) > LIST_MAX:
            break
        out += line
    return bytes(out)


def dlr_serve(sock, addr, cfg, store, hello_sent=False):
    sock.settimeout(5)
    ip = peer_str(addr)
    pw_flag = "1" if cfg["password_hash"] else "0"
    frame_send(sock, ("DLR_SERVER|%s|%s|%d" % (cfg["name"], pw_flag, PROTOCOL_VERSION)).encode())

    first = frame_recv(sock, 80)
    if not first.startswith(b"KEY:"):
        return                                   # a probe: read the hello and left
    try:
        key = base64.b64decode(first[4:], validate=True)
    except (binascii.Error, ValueError):
        return
    if len(key) != 32:
        return

    if cfg["password_hash"]:
        if LIMITER.blocked(ip):
            return
        frame_send(sock, bytes([MSG_AUTH_REQUEST]))
        sock.settimeout(30)
        supplied = frame_recv(sock, 256).decode("utf-8", "replace")
        if password_ok(cfg, supplied):
            frame_send(sock, bytes([MSG_HELLO_ACK]))
        else:
            LIMITER.fail(ip)
            frame_send(sock, bytes([MSG_ERROR]) + b"Authentication failed")
            time.sleep(1)                        # one thread per client: costs nobody else
            log("dlr: bad password from %s" % ip)
            return

    s = DlrSession(sock, AESGCM(key))
    chunk_size = min(cfg["chunk_size"], 60000)
    sock.settimeout(120)
    while True:
        try:
            mtype, body = s.recv()
        except (ConnectionError, socket.timeout, OSError):
            return
        arg = body[:127].decode("utf-8", "replace")
        if mtype == MSG_PKG_LIST:
            s.send(MSG_PKG_LIST, list_lines(store.list()))
        elif mtype == MSG_SEARCH_REQUEST:
            found = list_lines(store.search(arg))
            s.send(MSG_SEARCH_RESULT, found or b"NONE")
        elif mtype == MSG_PING:
            s.send(MSG_PONG, body)
        elif mtype == MSG_INSTALL_REQUEST:
            if not dlr_send_package(s, store, arg, chunk_size, ip):
                return
        # unknown types are ignored, as the C++ server does


def dlr_send_package(s, store, name, chunk_size, ip):
    """Returns False if the connection can no longer be used."""
    meta = store.get(name) if NAME_RE.match(name) else None
    if meta is None:
        msg = "Invalid package name" if not NAME_RE.match(name) else "Package not found: " + name
        s.send(MSG_INSTALL_ERROR, msg.encode())
        return True
    try:
        f = open(store.tar_path(name), "rb")
    except OSError:
        s.send(MSG_INSTALL_ERROR, b"Package archive is missing")
        return True
    with f:
        size = os.fstat(f.fileno()).st_size
        s.send(MSG_INSTALL_DATA, b"SIZE:%d" % size)      # no FORMAT field = tar
        digest, sent, batch = hashlib.sha256(), 0, bytearray()
        while True:
            block = f.read(chunk_size)
            if not block:
                break
            digest.update(block)
            sent += len(block)
            frame = s.seal(MSG_INSTALL_DATA, block)
            batch += struct.pack(">I", len(frame)) + frame
            if len(batch) >= 64 * 1024:                  # fewer syscalls than a send per chunk
                s.sock.sendall(batch)
                batch.clear()
        if batch:
            s.sock.sendall(batch)
        if sent != size:
            s.send(MSG_INSTALL_ERROR, b"Package changed during download - try again")
            return False
        s.send(MSG_INSTALL_END, digest.hexdigest().encode())
    log("dlr: sent %s (%d bytes) to %s" % (name, size, ip))
    return True


# =============================================================================
# public web pages and /api/package
# =============================================================================
CSS = """
:root{--bg:#f5f6f8;--fg:#14181f;--mut:#5a6473;--card:#fff;--line:#e0e4ea;--acc:#2f6bff;--acc2:#7a4dff;--on:#fff;
--code:#eef1f6;--ok:#16a34a;--warn:#b45309;--shadow:0 1px 2px #0000000d,0 6px 20px -8px #0000001f}
@media(prefers-color-scheme:dark){:root{--bg:#0d1016;--fg:#e8ebf1;--mut:#94a0b2;--card:#161a22;--line:#262c37;--acc:#7aa2ff;--acc2:#a78bfa;--on:#0b0e13;
--code:#1f2530;--ok:#4ade80;--warn:#fbbf24;--shadow:0 1px 2px #0006,0 8px 24px -10px #000a}}
*{box-sizing:border-box}html{scroll-behavior:smooth}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif;-webkit-font-smoothing:antialiased}
a{color:var(--acc);text-decoration:none}a:hover{text-decoration:underline}
.top{position:sticky;top:0;z-index:5;background:color-mix(in srgb,var(--bg) 82%,transparent);backdrop-filter:blur(10px);border-bottom:1px solid var(--line)}
.top div{max-width:60rem;margin:0 auto;padding:.7rem 1rem;display:flex;align-items:center;gap:1.1rem}
.brand{display:flex;align-items:center;gap:.55rem;font-weight:700;font-size:1.05rem;color:var(--fg);margin-right:auto}
.brand:hover{text-decoration:none}.brand svg{width:26px;height:26px}
.top a.nav{color:var(--mut);font-size:.95rem;padding:.2rem .1rem;border-bottom:2px solid transparent}
.top a.nav:hover,.top a.on{color:var(--fg);text-decoration:none;border-color:var(--acc)}
main{max-width:60rem;margin:0 auto;padding:1.6rem 1rem 3rem}
.hero{padding:2.2rem 1.4rem;border-radius:16px;color:#fff;background:linear-gradient(135deg,var(--acc),var(--acc2));box-shadow:var(--shadow);margin-bottom:1.2rem}
.hero h1{margin:0 0 .3rem;font-size:clamp(1.7rem,5vw,2.4rem);line-height:1.15}.hero p{margin:0;opacity:.92;max-width:34rem}
.hero .pill{display:inline-flex;align-items:center;gap:.4rem;background:#ffffff26;border-radius:99px;padding:.15rem .7rem;font-size:.82rem;margin-bottom:.9rem}
.dot{width:.55rem;height:.55rem;border-radius:50%;background:#4ade80;box-shadow:0 0 0 3px #4ade8040}
.grid{display:grid;gap:.8rem;grid-template-columns:repeat(auto-fit,minmax(10rem,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1rem 1.1rem;box-shadow:var(--shadow)}
.stat b{display:block;font-size:1.6rem;line-height:1.2}.stat span{color:var(--mut);font-size:.88rem}
h2{font-size:1.15rem;margin:2rem 0 .7rem}.mut{color:var(--mut)}
.step{display:flex;gap:.8rem;align-items:flex-start;margin:.55rem 0}
.step i{flex:none;width:1.7rem;height:1.7rem;border-radius:50%;background:var(--acc);color:var(--on);font:700 .85rem/1.7rem system-ui;text-align:center;font-style:normal;margin-top:.2rem}
.step div{flex:1;min-width:0}.step p{margin:0 0 .3rem}
.cmd{display:flex;align-items:center;gap:.5rem;background:var(--code);border-radius:9px;padding:.35rem .4rem .35rem .8rem;margin:.25rem 0}
.cmd code{flex:1;min-width:0;overflow-x:auto;white-space:nowrap;font:13.5px ui-monospace,SFMono-Regular,Menlo,monospace;scrollbar-width:thin}
button,.btn{font:inherit;font-size:.86rem;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:8px;padding:.28rem .7rem;cursor:pointer;white-space:nowrap}
button:hover,.btn:hover{border-color:var(--acc);text-decoration:none}
.btn.pri{background:var(--acc);color:var(--on);border-color:var(--acc);font-weight:600}
.tools{display:flex;gap:.6rem;flex-wrap:wrap;margin:.2rem 0 1rem}
.tools input,.tools select{font:inherit;padding:.5rem .8rem;border:1px solid var(--line);border-radius:10px;background:var(--card);color:var(--fg)}
.tools input{flex:1;min-width:12rem}.tools input:focus,.tools select:focus{outline:2px solid var(--acc);outline-offset:1px}
.pkg h3{margin:0;font-size:1.15rem}.pkg .head{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap}
.pkg .sz{margin-left:auto;color:var(--mut);font-size:.9rem}.pkg p.d{margin:.35rem 0 .6rem}
.tag{font-size:.74rem;padding:.04rem .55rem;border:1px solid var(--line);border-radius:99px;color:var(--mut)}
.tag.v{border-color:var(--acc);color:var(--acc)}
.acts{display:flex;gap:.5rem;flex-wrap:wrap;align-items:center;margin-top:.5rem}
details{margin-top:.6rem}summary{cursor:pointer;color:var(--mut);font-size:.92rem;user-select:none}
summary:hover{color:var(--fg)}
.tree,.tree ul{list-style:none;margin:.3rem 0 0;padding-left:1.1rem}.tree{padding-left:0;font:13px ui-monospace,monospace}
.tree li{display:flex;gap:.6rem;padding:.08rem 0}.tree li.f span:last-child{margin-left:auto;color:var(--mut);white-space:nowrap}
.tree summary{font:13px ui-monospace,monospace;color:var(--fg)}.tree li.f::before{content:"·";color:var(--mut)}
.hash{display:block;margin-top:.6rem;font:11.5px ui-monospace,monospace;color:var(--mut);word-break:break-all}
.empty{text-align:center;padding:2.5rem 1rem;color:var(--mut)}
.note{border-left:3px solid var(--warn);padding:.5rem .9rem;background:color-mix(in srgb,var(--warn) 9%,var(--card));border-radius:0 10px 10px 0;margin:.8rem 0;font-size:.92rem}
footer{max-width:60rem;margin:0 auto;padding:0 1rem 2rem;color:var(--mut);font-size:.85rem}
@media(max-width:34rem){.top div{gap:.7rem}.hero{padding:1.5rem 1.1rem}}
"""

ICON = ("<svg viewBox='0 0 32 32' aria-hidden='true'><rect width='32' height='32' rx='8' fill='#2f6bff'/>"
        "<path d='M16 6l8 4.5v9L16 24l-8-4.5v-9z' fill='none' stroke='#fff' stroke-width='2' stroke-linejoin='round'/>"
        "<path d='M8 10.5l8 4.5 8-4.5M16 15v9' fill='none' stroke='#fff' stroke-width='2' stroke-linejoin='round'/></svg>")
FAVICON = "data:image/svg+xml," + urllib.parse.quote(ICON.replace("aria-hidden='true'", "xmlns='http://www.w3.org/2000/svg'"))

# Small enhancement script: copy buttons, search and sort on /list. Everything
# works without it (the CSP only allows exactly this script, by hash).
PAGE_JS = """
(function(){
function copy(t,b){var done=function(){var o=b.textContent;b.textContent='Copied';setTimeout(function(){b.textContent=o},1200)};
 if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(t).then(done,function(){})}
 else{var a=document.createElement('textarea');a.value=t;a.style.position='fixed';a.style.opacity='0';document.body.appendChild(a);a.select();
 try{document.execCommand('copy');done()}catch(e){}document.body.removeChild(a)}}
document.querySelectorAll('[data-copy]').forEach(function(b){b.addEventListener('click',function(){copy(b.getAttribute('data-copy'),b)})});
var q=document.getElementById('q'),s=document.getElementById('sort'),list=document.getElementById('pkgs');if(!q||!list)return;
var cards=[].slice.call(list.querySelectorAll('.pkg')),cnt=document.getElementById('count'),none=document.getElementById('none');
function apply(){var t=q.value.trim().toLowerCase(),n=0;
 cards.forEach(function(c){var ok=!t||c.getAttribute('data-q').indexOf(t)>=0;c.hidden=!ok;if(ok)n++});
 cnt.textContent=n+' of '+cards.length;none.hidden=n>0}
function sort(){var k=s.value;cards.sort(function(a,b){return k=='size'?b.dataset.size-a.dataset.size:a.dataset.name.localeCompare(b.dataset.name)});
 cards.forEach(function(c){list.appendChild(c)})}
q.addEventListener('input',apply);s.addEventListener('change',sort);apply();
var ex=document.getElementById('expand');if(ex)ex.addEventListener('click',function(){var open=ex.getAttribute('data-o')!='1';
 document.querySelectorAll('.pkg details').forEach(function(d){d.open=open});ex.setAttribute('data-o',open?'1':'0');ex.textContent=open?'Collapse all':'Expand all'});
})();
"""
PAGE_JS_HASH = "sha256-" + base64.b64encode(hashlib.sha256(PAGE_JS.encode()).digest()).decode()
PAGE_CSP = ("default-src 'none'; style-src 'unsafe-inline'; img-src data:; script-src '%s'; "
            "base-uri 'none'; form-action 'none'; frame-ancestors 'none'" % PAGE_JS_HASH)


def page(title, body, cfg, active=""):
    def nav(href, label, key):
        return "<a class='nav%s' href='%s'>%s</a>" % (" on" if key == active else "", href, label)
    return ("<!doctype html><html lang=en><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<meta name=color-scheme content='light dark'><link rel=icon href=\"%s\">"
            "<title>%s</title><style>%s</style>"
            "<header class=top><div><a class=brand href='/'>%s<span>%s</span></a>%s%s%s</div></header>"
            "<main>%s</main><footer>dlrd &middot; Deliver server for Minimal-OS</footer>"
            "<script>%s</script></html>"
            % (FAVICON, html.escape(title), CSS, ICON, html.escape(cfg["name"]),
               nav("/", "Home", "home"), nav("/list", "Packages", "list"), nav("/api/package", "API", "api"),
               body, PAGE_JS)).encode()


def safe_host(handler, cfg):
    host = (cfg.get("public_host") or handler.headers.get("Host") or "").strip()
    return host if re.fullmatch(r"[A-Za-z0-9.\-\[\]:]{1,255}", host) else "this-server"


def bare_host(host):
    return host if host.startswith("[") else host.split(":")[0]


def origin(handler, cfg):
    """scheme://host[:port] as the visitor reached the site."""
    scheme = "https" if handler.headers.get("X-Forwarded-Proto") == "https" else "http"
    host = (handler.headers.get("Host") or "").strip()
    host = host if re.fullmatch(r"[A-Za-z0-9.\-\[\]:]{1,255}", host) else "this-server"
    return "%s://%s" % (scheme, host)


def cmd_box(text):
    t = html.escape(text, quote=True)
    return "<div class=cmd><code>%s</code><button type=button data-copy=\"%s\">Copy</button></div>" % (t, t)


def dlr_add_cmd(cfg, host):
    port = cfg["dlr_port"]
    return "dlr add %s%s" % (bare_host(host), "" if port in (0, 4242) else ":%d" % port)


def render_home(h, cfg, store):
    metas = store.list()
    host = safe_host(h, cfg)
    base = origin(h, cfg)
    total = sum(m["size"] for m in metas)
    first = html.escape(metas[0]["name"]) if metas else "&lt;package&gt;"
    first_raw = metas[0]["name"] if metas else "<package>"
    lock = ("<div class=note>This server is password protected. Add <code>--password &lt;pw&gt;</code> to "
            "<code>dlr list</code> and <code>dlr install</code>, or send it as an "
            "<code>X-DLR-Password</code> header to the API.</div>") if cfg["password_hash"] else ""
    latest = ""
    for m in sorted(metas, key=lambda m: -m["built"])[:4]:
        latest += ("<a class=card href='/list#%s' style='color:inherit'><b>%s</b> <span class=tag>%s</span>"
                   "<div class=mut style='font-size:.9rem'>%s</div></a>"
                   % (html.escape(m["name"]), html.escape(m["name"]), html.escape(m["version"] or "-"),
                      html.escape(m["description"] or human(m["size"]))))
    body = (
        "<section class=hero><span class=pill><span class=dot></span>Online</span>"
        "<h1>%(name)s</h1><p>Packages for Minimal-OS, delivered with dlr. Browse what is on offer, then install it "
        "with one command.</p></section>"
        "<div class=grid><div class='card stat'><b>%(n)d</b><span>packages</span></div>"
        "<div class='card stat'><b>%(size)s</b><span>on offer</span></div>"
        "<div class='card stat'><b>%(auth)s</b><span>access</span></div>"
        "<div class='card stat'><b>dlr v%(proto)d</b><span>protocol</span></div></div>%(lock)s"
        "<h2>Install on Minimal-OS</h2><div class=card>"
        "<div class=step><i>1</i><div><p>Get a network address, once per boot:</p>%(c1)s</div></div>"
        "<div class=step><i>2</i><div><p>Add this server:</p>%(c2)s</div></div>"
        "<div class=step><i>3</i><div><p>See what is available, then install:</p>%(c3)s%(c4)s</div></div></div>"
        "%(latest)s%(admin)s"
        "<h2>For scripts and tools</h2><div class=card>"
        "<p style='margin-top:0'>Every package is a plain HTTP download. The checksum comes in the "
        "<code>X-DLR-SHA256</code> header.</p>%(a1)s%(a2)s%(a3)s"
        "<p class=mut style='margin-bottom:0'><a href='/api/package'>Open the JSON list &rarr;</a></p></div>"
        % dict(name=html.escape(cfg["name"]), n=len(metas), size=html.escape(human(total)),
               auth="Password" if cfg["password_hash"] else "Open", proto=PROTOCOL_VERSION, lock=lock,
               c1=cmd_box("dhcp"), c2=cmd_box(dlr_add_cmd(cfg, host)), c3=cmd_box("dlr list"),
               c4=cmd_box("dlr install " + first_raw),
               admin=("<h2>Add packages</h2><div class=card>Open the <a href='http://127.0.0.1:%d/'>admin window</a> "
                      "to drag a folder in, edit settings and manage files.</div>" % cfg["admin_port"])
               if cfg["admin_port"] and bare_host(h.headers.get("Host") or "") in LOOPBACK_HOSTS else "",
               latest=("<h2>Latest packages</h2><div class=grid>%s</div>"
                       "<p><a href='/list'>Browse all packages and their files &rarr;</a></p>" % latest) if metas else "",
               a1=cmd_box("curl %s/api/package" % base),
               a2=cmd_box("curl -O %s/api/package/%s" % (base, urllib.parse.quote(first_raw))),
               a3=cmd_box("curl %s/api/package/%s/info" % (base, urllib.parse.quote(first_raw)))))
    return page(cfg["name"], body, cfg, "home")


def render_tree(files):
    root = {}
    for f in files:
        node = root
        parts = f["path"].split("/")
        for p in parts[:-1]:
            node = node.setdefault(p + "/", {})
        node[parts[-1]] = f["size"]

    def walk(node, depth):
        out = []
        for key in sorted(node, key=lambda k: (not k.endswith("/"), k.lower())):
            val = node[key]
            if isinstance(val, dict):
                out.append("<li><details%s><summary>%s</summary><ul>%s</ul></details></li>"
                           % (" open" if depth == 0 else "", html.escape(key), walk(val, depth + 1)))
            else:
                out.append("<li class=f><span>%s</span><span>%s</span></li>" % (html.escape(key), human(val)))
        return "".join(out)
    return "<ul class=tree>%s</ul>" % walk(root, 0)


def render_list(h, cfg, store):
    metas = store.list()
    host = safe_host(h, cfg)
    if not metas:
        return page("Packages", "<h1>Packages</h1><div class='card empty'>Nothing is published yet.</div>",
                    cfg, "list")
    cards = []
    for m in metas:
        tags = "".join("<span class=tag>%s</span>" % html.escape(t) for t in (m["arch"], m["os"]) if t)
        url = urllib.parse.quote(m["name"])
        cards.append(
            "<article class='card pkg' id=\"%(id)s\" data-name=\"%(idl)s\" data-size=\"%(raw)d\" data-q=\"%(q)s\">"
            "<div class=head><h3><a href='/api/package/%(url)s' title='Download %(name)s'>%(name)s</a></h3>%(ver)s%(tags)s<span class=sz>%(size)s</span></div>"
            "<p class=d>%(desc)s</p>"
            "%(cmd)s<div class=acts><a class='btn pri' href='/api/package/%(url)s'>Download</a>"
            "<a class=btn href='/api/package/%(url)s/info'>JSON</a></div>"
            "<details><summary>%(count)d files &middot; %(total)s unpacked</summary>%(tree)s</details>"
            "<span class=hash>sha256 %(sha)s</span></article>"
            % dict(id=html.escape(m["name"], quote=True), idl=html.escape(m["name"].lower(), quote=True),
                   raw=m["size"], q=html.escape((m["name"] + " " + m["description"]).lower(), quote=True),
                   name=html.escape(m["name"]),
                   ver=("<span class='tag v'>%s</span>" % html.escape(m["version"])) if m["version"] else "",
                   tags=tags, size=html.escape(human(m["size"])),
                   desc=html.escape(m["description"]) or "<span class=mut>No description.</span>",
                   cmd=cmd_box("dlr install %s" % m["name"]), url=url, count=m["file_count"],
                   total=html.escape(human(m["total_bytes"])), tree=render_tree(m["files"]), sha=m["sha256"]))
    body = ("<h1 style='margin:.2rem 0 .8rem'>Packages</h1>"
            "<div class=tools><input id=q type=search placeholder='Search packages' autocomplete=off "
            "aria-label='Search packages'><select id=sort aria-label='Sort'><option value=name>Name</option>"
            "<option value=size>Largest first</option></select><button id=expand type=button>Expand all</button>"
            "<span class=mut style='align-self:center' id=count></span></div>"
            "<div id=pkgs style='display:grid;gap:.9rem'>%s</div>"
            "<div id=none class='card empty' hidden>No package matches your search.</div>" % "".join(cards))
    return page("Packages", body, cfg, "list")


class PublicHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "dlrd"
    sys_version = ""
    timeout = 20

    # context set by the worker
    store = None
    config = None

    def log_message(self, fmt, *args):
        log("http: %s %s" % (self.address_string(), fmt % args))

    # -- plumbing
    def _send(self, code, body=b"", ctype="text/html; charset=utf-8", extra=None, head=False):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        if ctype.startswith("text/html"):
            self.send_header("Content-Security-Policy", PAGE_CSP)
        udp = self.config.get()["udp_public"]
        if udp and re.fullmatch(r"[A-Za-z0-9.\-\[\]:]{1,255}", udp):
            self.send_header("X-DLR-UDP", udp)           # where the UDP side lives
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def _json(self, obj, code=200, head=False):
        self._send(code, json.dumps(obj, indent=1).encode(), "application/json", head=head)

    def _authorised(self, cfg):
        if not cfg["password_hash"]:
            return True
        ip = self.client_address[0]
        if LIMITER.blocked(ip):
            self._send(429, b"Too many failed attempts\n", "text/plain; charset=utf-8",
                       {"Retry-After": "60"})
            return False
        supplied = self.headers.get("X-DLR-Password")
        auth = self.headers.get("Authorization", "")
        if supplied is None and auth[:6].lower() == "basic ":
            try:
                supplied = base64.b64decode(auth[6:]).decode("utf-8", "replace").partition(":")[2]
            except (binascii.Error, ValueError):
                supplied = None
        elif supplied is None and auth[:7].lower() == "bearer ":
            supplied = auth[7:].strip()
        if supplied is not None and password_ok(cfg, supplied):
            return True
        if supplied is not None:
            LIMITER.fail(ip)
        self._send(401, b"Password required\n", "text/plain; charset=utf-8",
                   {"WWW-Authenticate": 'Basic realm="dlr", charset="UTF-8"'})
        return False

    # -- verbs
    def do_HEAD(self):
        self._route(head=True)

    def do_GET(self):
        self._route(head=False)

    def _method_not_allowed(self):
        self._send(405, b"GET and HEAD only\n", "text/plain; charset=utf-8", {"Allow": "GET, HEAD"})

    do_POST = do_PUT = do_DELETE = do_PATCH = _method_not_allowed

    def _route(self, head):
        cfg = self.config.get()
        url = urllib.parse.urlsplit(self.path)
        path = urllib.parse.unquote(url.path)
        query = urllib.parse.parse_qs(url.query)
        store = self.store

        if path == "/api/endpoint":                  # public: how to reach every transport
            host = safe_host(self, cfg)
            udp = cfg["udp_public"] or None
            return self._json({"name": cfg["name"], "protocol": PROTOCOL_VERSION,
                               "password": bool(cfg["password_hash"]),
                               "dlr": {"host": bare_host(host), "port": cfg["dlr_port"]},
                               "udp": ({"host": udp.rsplit(":", 1)[0], "port": int(udp.rsplit(":", 1)[1])}
                                       if udp and re.fullmatch(r"[^:]+:\d{1,5}", udp) else None),
                               "http": origin(self, cfg)}, head=head)
        if path == "/favicon.ico":
            return self._send(204, b"", "image/x-icon", head=head)
        if path == "/":
            return self._send(200, render_home(self, cfg, store), head=head)
        if path == "/list":
            if not self._authorised(cfg):
                return
            if query.get("format") == ["json"] or "application/json" in self.headers.get("Accept", ""):
                return self._json(self._public_list(store.list()), head=head)
            return self._send(200, render_list(self, cfg, store), head=head)

        if path == "/api/package" or path == "/api/package/":
            if not self._authorised(cfg):
                return
            if "name" in query:                       # /api/package?name=x downloads too
                return self._download(query["name"][0], cfg, head)
            found = store.search(query["q"][0]) if "q" in query else store.list()
            return self._json({"server": cfg["name"], "protocol": PROTOCOL_VERSION,
                               "packages": self._public_list(found)}, head=head)

        m = re.fullmatch(r"/api/package/([^/]+)(/info)?", path)
        if m:
            if not self._authorised(cfg):
                return
            if m.group(2):
                meta = store.get(m.group(1))
                if meta is None:
                    return self._json({"error": "Package not found: " + m.group(1)}, 404, head)
                return self._json(self._public_meta(meta, files=True), head=head)
            return self._download(m.group(1), cfg, head)

        self._send(404, b"Not found\n", "text/plain; charset=utf-8", head=head)

    @staticmethod
    def _public_meta(m, files=False):
        out = {k: m[k] for k in ("name", "version", "description", "arch", "os", "format",
                                 "size", "sha256", "file_count", "total_bytes")}
        out["download"] = "/api/package/" + urllib.parse.quote(m["name"])
        if files:
            out["files"] = m["files"]
        return out

    def _public_list(self, metas):
        return [self._public_meta(m) for m in metas]

    def _download(self, name, cfg, head):
        store = self.store
        meta = store.get(name)
        if meta is None:
            return self._json({"error": "Package not found: " + name}, 404, head)
        try:
            f = open(store.tar_path(name), "rb")
        except OSError:
            return self._json({"error": "Package archive is missing"}, 404, head)
        with f:
            size = os.fstat(f.fileno()).st_size
            if size != meta["size"]:                  # rebuilt between the two reads
                return self._send(503, b"Package is being rebuilt, retry\n", "text/plain; charset=utf-8",
                                  {"Retry-After": "2"}, head)
            self.send_response(200)
            self.send_header("Content-Type", "application/x-tar")
            self.send_header("Content-Length", str(size))
            self.send_header("Content-Disposition", 'attachment; filename="%s.tar"' % name)
            self.send_header("X-DLR-SHA256", meta["sha256"])
            self.send_header("X-DLR-Format", "tar")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            if head:
                return
            while True:
                block = f.read(1 << 20)
                if not block:
                    break
                self.wfile.write(block)
        log("http: sent %s (%d bytes) to %s" % (name, size, self.client_address[0]))



# =============================================================================
# dlr over UDP ("DLU1"): for networks where only UDP gets through (playit.gg)
# =============================================================================
# Stateless, client-driven, so loss needs no server memory: the client asks for
# numbered chunks and re-asks for any that do not arrive.
#
#   datagram  = "DLU1" op(1) body
#   HELLO  0x01 (empty)                      -> 0x81 cookie(16) flags(1: 1=password) name
#   every other request body starts with  cookie(16) proof(16)  then:
#   PING   0x02 echo                          -> 0x82 echo
#   LIST   0x03 page(u16)                     -> 0x83 page(u16) pages(u16) "name|ver|desc\n"...
#   SEARCH 0x04 page(u16) query               -> 0x83 ...  ("NONE" if nothing)
#   INFO   0x05 name                          -> 0x85 size(u64) sha256(32) format(u8, 0=tar)
#   GET    0x06 nlen(u8) name offset(u64) n(u8) -> n x 0x86 offset(u64) data(<=1000)
#   errors                                    -> 0xFF text
#
# cookie = HMAC(server secret, client address, 5-minute window)[:16]. Nothing but
# HELLO is answered without one, so a spoofed source address cannot make this
# server flood a victim (a HELLO reply is tiny). proof, when a password is set,
# is HMAC-SHA256(sha256(password), cookie || op || body)[:16]. It keeps the
# password off the wire; like the TCP protocol it does not hide the data.
UDP_MAGIC = b"DLU1"
UDP_CHUNK = 1000
UDP_PAGE = 1000
(U_HELLO, U_PING, U_LIST, U_SEARCH, U_INFO, U_GET) = range(1, 7)
U_ERR = 0xFF
_UDP_SECRET = secrets.token_bytes(32)          # made before the fork, so workers share it


def udp_cookie(ip, window):
    return hmac.new(_UDP_SECRET, ("%s|%d" % (ip, window)).encode(), hashlib.sha256).digest()[:16]


def udp_err(text):
    return UDP_MAGIC + bytes([U_ERR]) + text.encode()[:200]


def udp_pages(metas):
    pages, cur = [], b""
    for m in metas:
        clean = lambda t: re.sub(r"[|\r\n\t]", " ", t)
        line = ("%s|%s|%s\n" % (m["name"], clean(m["version"])[:31], clean(m["description"])[:159])).encode()
        if cur and len(cur) + len(line) > UDP_PAGE:
            pages.append(cur)
            cur = b""
        cur += line
    if cur:
        pages.append(cur)
    return pages


def udp_handle(data, ip, cfg, store):
    """One request datagram in, a list of reply datagrams out."""
    if len(data) < 5 or data[:4] != UDP_MAGIC:
        return []
    op, body = data[4], data[5:]
    now = int(time.time() // 300)
    if op == U_HELLO:
        return [UDP_MAGIC + bytes([0x80 | U_HELLO]) + udp_cookie(ip, now)
                + bytes([1 if cfg["password_hash"] else 0]) + cfg["name"].encode()]
    if op not in (U_PING, U_LIST, U_SEARCH, U_INFO, U_GET) or len(body) < 32:
        return [udp_err("bad request")]
    cookie, proof, body = body[:16], body[16:32], body[32:]
    if cookie != udp_cookie(ip, now) and cookie != udp_cookie(ip, now - 1):
        return [udp_err("stale cookie")]
    if cfg["password_hash"]:
        if LIMITER.blocked(ip):
            return []
        want = hmac.new(bytes.fromhex(cfg["password_hash"]), cookie + bytes([op]) + body,
                        hashlib.sha256).digest()[:16]
        if not hmac.compare_digest(proof, want):
            LIMITER.fail(ip)
            return [udp_err("Authentication failed")]

    if op == U_PING:
        return [UDP_MAGIC + bytes([0x80 | U_PING]) + body[:64]]
    if op in (U_LIST, U_SEARCH):
        if len(body) < 2:
            return [udp_err("bad request")]
        page = struct.unpack(">H", body[:2])[0]
        metas = store.search(body[2:130].decode("utf-8", "replace")) if op == U_SEARCH else store.list()
        pages = udp_pages(metas) or [b"NONE" if op == U_SEARCH else b""]
        if page >= len(pages):
            return [udp_err("no such page")]
        return [UDP_MAGIC + bytes([0x80 | U_LIST]) + struct.pack(">HH", page, len(pages)) + pages[page]]
    if op == U_INFO:
        meta = store.get(body.decode("utf-8", "replace"))
        if meta is None:
            return [udp_err("Package not found")]
        return [UDP_MAGIC + bytes([0x80 | U_INFO]) + struct.pack(">Q", meta["size"])
                + bytes.fromhex(meta["sha256"]) + b"\x00"]
    if op == U_GET:
        if len(body) < 1 or len(body) < 1 + body[0] + 9:
            return [udp_err("bad request")]
        nlen = body[0]
        name = body[1:1 + nlen].decode("utf-8", "replace")
        offset, count = struct.unpack(">QB", body[1 + nlen:1 + nlen + 9])
        meta = store.get(name)
        if meta is None:
            return [udp_err("Package not found")]
        try:
            fd = os.open(store.tar_path(name), os.O_RDONLY)
        except OSError:
            return [udp_err("Package archive is missing")]
        try:
            if os.fstat(fd).st_size != meta["size"]:
                return [udp_err("Package is being rebuilt, retry")]
            out = []
            for i in range(min(count, 16)):
                at = offset + i * UDP_CHUNK
                chunk = os.pread(fd, UDP_CHUNK, at) if at < meta["size"] else b""
                out.append(UDP_MAGIC + bytes([0x80 | U_GET]) + struct.pack(">Q", at) + chunk)
                if not chunk:
                    break
            return out
        finally:
            os.close(fd)
    return []


def udp_loop(sock, config, store):
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except OSError:
            time.sleep(0.05)
            continue
        try:
            for reply in udp_handle(data, addr[0], config.get(), store):
                sock.sendto(reply, addr)
        except OSError:
            pass
        except Exception as e:                          # a bad datagram must not kill the loop
            log("udp: error from %s: %r" % (addr[0], e))


# =============================================================================
# worker processes: accept on the shared sockets
# =============================================================================
class TlsContexts:
    def __init__(self):
        self._key, self._ctx = None, None

    def get(self, cfg):
        cert, key = cfg["cert_file"], cfg["key_file"]
        try:
            stamp = (cert, key, os.stat(cert).st_mtime_ns, os.stat(key).st_mtime_ns)
        except OSError:
            return None
        if stamp != self._key:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            ctx.load_cert_chain(cert, key)             # re-read after certbot renews
            self._ctx, self._key = ctx, stamp
        return self._ctx


def worker_main(listeners, data_dir, config_path):
    """listeners: [(kind, socket)] with kind in dlr/http/https."""
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    config, store, tls = Config(config_path), Store(data_dir), TlsContexts()
    active = threading.BoundedSemaphore(10 ** 6)
    counter = {"n": 0}
    counter_lock = threading.Lock()

    class Handler(PublicHandler):
        pass
    Handler.store, Handler.config = store, config

    def serve_http(conn, addr):
        class Srv:                                    # what BaseHTTPRequestHandler wants
            server_name, server_port = "dlrd", 0
        Handler(conn, addr, Srv())

    def handle(kind, conn, addr):
        try:
            cfg = config.get()
            conn.settimeout(10)
            if kind == "https":
                ctx = tls.get(cfg)
                if ctx is None:
                    return
                conn = ctx.wrap_socket(conn, server_side=True)
            if kind == "dlr":
                dlr_serve(conn, addr, cfg, store)
            elif cfg["sniff_dlr_on_http"] and not select.select([conn], [], [], 0.4)[0]:
                dlr_serve(conn, addr, cfg, store)      # said nothing: a dlr client listens first
            else:
                serve_http(conn, addr)
        except (ConnectionError, socket.timeout, ssl.SSLError, OSError):
            pass
        except Exception as e:                          # one bad client must not take the worker down
            log("%s: error from %s: %r" % (kind, peer_str(addr), e))
        finally:
            try:
                conn.close()
            except OSError:
                pass
            with counter_lock:
                counter["n"] -= 1

    def accept_loop(kind, sock):
        while True:
            try:
                conn, addr = sock.accept()
            except OSError:
                time.sleep(0.05)
                continue
            with counter_lock:
                over = counter["n"] >= config.get()["max_connections"]
                if not over:
                    counter["n"] += 1
            if over:
                conn.close()
                continue
            threading.Thread(target=handle, args=(kind, conn, addr), daemon=True).start()

    threads = [threading.Thread(target=udp_loop if k == "udp" else accept_loop,
                                args=(s, config, store) if k == "udp" else (k, s), daemon=True)
               for k, s in listeners]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def bind(port, what):
    """Dual-stack listener on `port`, or None with a helpful message."""
    for family, host in ((socket.AF_INET6, "::"), (socket.AF_INET, "0.0.0.0")):
        try:
            s = socket.socket(family, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if family == socket.AF_INET6:
                s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            s.bind((host, port))
            s.listen(256)
            return s
        except PermissionError:
            s.close()
            log("cannot listen on port %d for %s: ports below 1024 need privileges. Either run "
                "`sudo setcap cap_net_bind_service=+ep $(readlink -f $(which python3))`, or pick a high "
                "port in Settings and forward the public port to it on your router." % (port, what))
            return None
        except OSError as e:
            s.close()
            if family == socket.AF_INET:
                log("cannot listen on port %d for %s: %s" % (port, what, e))
    return None


def bind_udp(port, what):
    for family, host in ((socket.AF_INET6, "::"), (socket.AF_INET, "0.0.0.0")):
        try:
            s = socket.socket(family, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if family == socket.AF_INET6:
                s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            s.bind((host, port))
            return s
        except OSError as e:
            s.close()
            if family == socket.AF_INET:
                log("cannot listen on UDP port %d for %s: %s" % (port, what, e))
    return None


# =============================================================================
# admin window (master process, web UI)
# =============================================================================
ADMIN_HTML = r"""<!doctype html><html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>dlrd admin</title>
<style>
:root{--bg:#f6f7f9;--fg:#1b1f24;--mut:#5b6572;--card:#fff;--line:#dfe3e8;--acc:#2563eb;--on:#fff;--bad:#c62828;--ok:#2e7d32;--code:#eef1f5}
@media(prefers-color-scheme:dark){:root{--bg:#101318;--fg:#e6e9ee;--mut:#97a1ae;--card:#181c23;--line:#2a303a;--acc:#6ea0ff;--on:#0b0e13;--bad:#ff6b6b;--ok:#6fd47b;--code:#222833}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif}
header{display:flex;align-items:center;gap:.75rem;padding:.7rem 1.2rem;border-bottom:1px solid var(--line);background:var(--card);position:sticky;top:0;z-index:2}
header h1{font-size:1.1rem;margin:0;margin-right:auto}header small{color:var(--mut)}
main{max-width:62rem;margin:0 auto;padding:1.2rem 1rem 4rem}
button,.btn{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--fg);border-radius:8px;padding:.35rem .8rem;cursor:pointer}
button.pri{background:var(--acc);color:var(--on);border-color:var(--acc)}button.del{color:var(--bad)}button:disabled{opacity:.5;cursor:default}
#drop{border:2px dashed var(--line);border-radius:12px;padding:2rem 1rem;text-align:center;color:var(--mut);background:var(--card);transition:.15s}
#drop.over{border-color:var(--acc);color:var(--fg);background:color-mix(in srgb,var(--acc) 8%,var(--card))}
#drop b{color:var(--fg);font-size:1.05rem}.row{display:flex;gap:.6rem;align-items:center;flex-wrap:wrap;justify-content:center;margin-top:.8rem}
input[type=text],input[type=password],input[type=number]{font:inherit;padding:.35rem .6rem;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);width:100%}
label{display:block;font-size:.85rem;color:var(--mut);margin:.7rem 0 .2rem}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;margin:.7rem 0;padding:.8rem 1rem}
.pk{display:flex;gap:.8rem;align-items:baseline;flex-wrap:wrap}.pk .nm{font-weight:600;font-size:1.02rem}.mut{color:var(--mut)}.sp{margin-left:auto;display:flex;gap:.4rem}
table{width:100%;border-collapse:collapse;font-size:.88rem;margin-top:.5rem}td{padding:.12rem .3rem;border-top:1px solid var(--line)}td.n{text-align:right;color:var(--mut);white-space:nowrap}
h2{font-size:1rem;margin:1.6rem 0 .3rem}#msg{min-height:1.4em;margin:.6rem 0;font-size:.9rem}.err{color:var(--bad)}.okc{color:var(--ok)}
progress{width:100%;height:.6rem;margin-top:.6rem}
dialog{border:1px solid var(--line);border-radius:12px;background:var(--card);color:var(--fg);padding:1.2rem 1.3rem;width:min(30rem,94vw);max-height:92vh;overflow:auto}
dialog::backdrop{background:#0008}.two{display:grid;grid-template-columns:1fr 1fr;gap:.8rem}.chk{display:flex;gap:.5rem;align-items:center;margin-top:.8rem;color:var(--fg);font-size:.9rem}
.chk input{width:auto}.foot{display:flex;gap:.5rem;justify-content:flex-end;margin-top:1.2rem;position:sticky;bottom:-1.2rem;background:var(--card);padding:.6rem 0}.note{font-size:.8rem;color:var(--mut);margin-top:.5rem}
code{background:var(--code);padding:.1em .35em;border-radius:5px}
</style>
<header><h1 id=title>dlrd</h1><small id=meta></small><button id=openset>⚙ Settings</button></header>
<main>
<div id=drop><b>Drop a folder here to publish it as a package</b><br>
<span>or files — they become one package. The folder name is the package name.</span>
<div class=row><input type=text id=pname placeholder="package name (for loose files)" style="max-width:15rem">
<button id=pickdir>Choose folder…</button><button id=pickfiles>Choose files…</button>
<label class=chk style="margin:0"><input type=checkbox id=replace> replace if it exists</label></div>
<progress id=prog hidden></progress></div>
<input type=file id=fdir webkitdirectory hidden><input type=file id=ffiles multiple hidden>
<div id=msg></div>
<h2>Packages</h2><div id=pkgs></div>
</main>
<dialog id=dlg><form method=dialog id=setform>
<h3 style="margin:0 0 .4rem">Settings</h3>
<label>Server name</label><input type=text name=name maxlength=63>
<label>Client password <span class=mut id=pwstate></span></label><input type=password name=password placeholder="leave empty to keep" autocomplete=new-password>
<label class=chk><input type=checkbox name=clear_password> remove the password (open server)</label>
<div class=two><div><label>dlr port (TCP)</label><input type=number name=dlr_port min=0 max=65535></div>
<div><label>Web port (HTTP)</label><input type=number name=http_port min=0 max=65535></div>
<div><label>HTTPS port</label><input type=number name=https_port min=0 max=65535></div>
<div><label>UDP port</label><input type=number name=udp_port min=0 max=65535></div>
<div><label>Worker processes (0 = all cores)</label><input type=number name=workers min=0 max=256></div></div>
<label>Public host name (shown in install commands)</label><input type=text name=public_host placeholder="xlabs.duckdns.org">
<label>TLS certificate file (fullchain.pem)</label><input type=text name=cert_file>
<label>TLS key file (privkey.pem)</label><input type=text name=key_file>
<label>Public UDP address (host:port, e.g. from playit.gg)</label><input type=text name=udp_public placeholder="name.gl.at.ply.gg:12345">
<label class=chk><input type=checkbox name=sniff_dlr_on_http> also answer dlr clients on the web ports</label>
<label class=chk><input type=checkbox name=broadcast> announce on the LAN (UDP discovery)</label>
<div class=two><div><label>Admin window port</label><input type=number name=admin_port min=0 max=65535></div>
<div><label>Admin password <span class=mut id=apwstate></span></label><input type=password name=admin_password placeholder="leave empty to keep" autocomplete=new-password></div></div>
<p class=note>Ports, workers and TLS files apply after a restart. Name, password and packages apply immediately.</p>
<div class=foot><button type=button id=setcancel>Cancel</button><button type=button id=setrestart>Save &amp; restart</button><button class=pri type=submit value=save>Save</button></div>
</form></dialog>
<script>
const $=s=>document.querySelector(s);
const H={'X-DLR-Admin':'1'};
const fmt=n=>n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KiB':n<1073741824?(n/1048576).toFixed(1)+' MiB':(n/1073741824).toFixed(2)+' GiB';
function say(t,cls){const m=$('#msg');m.textContent=t;m.className=cls||''}
async function api(path,body){
  const r=await fetch('/admin/api/'+path,{method:body===undefined?'GET':'POST',headers:body===undefined?H:{...H,'Content-Type':'application/json'},body:body===undefined?undefined:JSON.stringify(body)});
  const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||r.statusText);return j}
let state=null;
async function refresh(){
  state=await api('state');
  $('#title').textContent=state.config.name;
  $('#meta').textContent=state.workers+' worker'+(state.workers==1?'':'s')+' · dlr :'+state.config.dlr_port+' · web :'+state.config.http_port;
  const box=$('#pkgs');box.textContent='';
  if(!state.packages.length){box.innerHTML='<div class="card mut">Nothing published yet. Drop a folder above.</div>'}
  for(const p of state.packages){
    const c=document.createElement('div');c.className='card';
    const head=document.createElement('div');head.className='pk';
    const nm=document.createElement('span');nm.className='nm';nm.textContent=p.name;
    const v=document.createElement('span');v.className='mut';v.textContent=[p.version,p.arch,p.os].filter(Boolean).join(' · ');
    const sz=document.createElement('span');sz.className='mut';sz.textContent=p.file_count+' files · '+fmt(p.size);
    const sp=document.createElement('span');sp.className='sp';
    const rb=document.createElement('button');rb.textContent='Rebuild';rb.onclick=()=>act('rebuild',{pkg:p.name},'Rebuilt '+p.name);
    const dl=document.createElement('button');dl.textContent='Delete';dl.className='del';
    dl.onclick=()=>{if(confirm('Delete package '+p.name+'?'))act('delete',{pkg:p.name},'Deleted '+p.name)};
    sp.append(rb,dl);head.append(nm,v,sz,sp);c.append(head);
    if(p.description){const d=document.createElement('div');d.className='mut';d.textContent=p.description;c.append(d)}
    const det=document.createElement('details');const sm=document.createElement('summary');sm.textContent='Files';det.append(sm);
    const t=document.createElement('table');
    for(const f of p.files){const r=t.insertRow();r.insertCell().textContent=f.path;const n=r.insertCell();n.className='n';n.textContent=fmt(f.size)}
    det.append(t);c.append(det);box.append(c)}
}
async function act(path,body,ok){try{await api(path,body);say(ok,'okc');await refresh()}catch(e){say(e.message,'err')}}
// ---- dropping / choosing
async function walk(entry,prefix,out){
  if(entry.isFile){await new Promise(res=>entry.file(f=>{out.push({path:prefix+entry.name,file:f});res()},res))}
  else if(entry.isDirectory){const rd=entry.createReader();let batch;
    do{batch=await new Promise(res=>rd.readEntries(res,()=>res([])));for(const e of batch)await walk(e,prefix+entry.name+'/',out)}while(batch.length)}
}
async function fromDrop(dt){
  const items=[...dt.items].map(i=>i.webkitGetAsEntry&&i.webkitGetAsEntry()).filter(Boolean);
  const out=[];let name='';
  if(items.length==1&&items[0].isDirectory){name=items[0].name;await walk(items[0],'',out);out.forEach(o=>o.path=o.path.slice(name.length+1))}
  else for(const e of items)await walk(e,'',out);
  return {name,files:out}}
function fromInput(list){
  const files=[...list];let name='';
  if(files.length&&files[0].webkitRelativePath){name=files[0].webkitRelativePath.split('/')[0];
    return {name,files:files.map(f=>({path:f.webkitRelativePath.split('/').slice(1).join('/'),file:f}))}}
  return {name:'',files:files.map(f=>({path:f.name,file:f}))}}
async function publish({name,files}){
  files=files.filter(f=>f.path&&!f.path.split('/').some(p=>p.startsWith('.')));
  name=name||$('#pname').value.trim();
  if(!files.length){say('No files found to upload.','err');return}
  if(!/^[A-Za-z0-9_-][A-Za-z0-9._-]{0,62}$/.test(name)){say('Give the package a name (letters, digits, . _ -) first.','err');$('#pname').focus();return}
  const total=files.reduce((a,f)=>a+f.file.size,0);let done=0;const pr=$('#prog');pr.hidden=false;pr.max=total||1;pr.value=0;
  try{
    await api('begin',{pkg:name,replace:$('#replace').checked});
    for(const f of files){
      say('Uploading '+f.path+' …');
      const r=await fetch('/admin/api/upload?pkg='+encodeURIComponent(name)+'&path='+encodeURIComponent(f.path),{method:'POST',headers:H,body:f.file});
      if(!r.ok)throw new Error((await r.json().catch(()=>({}))).error||('upload failed: '+f.path));
      done+=f.file.size;pr.value=done}
    say('Building archive …');await api('finalize',{pkg:name,replace:$('#replace').checked});
    say('Published '+name+' ('+files.length+' files).','okc');$('#pname').value='';await refresh()
  }catch(e){say(e.message,'err');try{await api('abort',{pkg:name})}catch(_){}}
  pr.hidden=true}
const drop=$('#drop');
['dragenter','dragover'].forEach(t=>drop.addEventListener(t,e=>{e.preventDefault();drop.classList.add('over')}));
['dragleave','drop'].forEach(t=>drop.addEventListener(t,e=>{e.preventDefault();drop.classList.remove('over')}));
drop.addEventListener('drop',async e=>{publish(await fromDrop(e.dataTransfer))});
$('#pickdir').onclick=()=>$('#fdir').click();$('#pickfiles').onclick=()=>$('#ffiles').click();
$('#fdir').onchange=e=>{publish(fromInput(e.target.files));e.target.value=''};
$('#ffiles').onchange=e=>{publish(fromInput(e.target.files));e.target.value=''};
window.addEventListener('dragover',e=>e.preventDefault());window.addEventListener('drop',e=>e.preventDefault());
// ---- settings
const dlg=$('#dlg'),form=$('#setform');
$('#openset').onclick=()=>{const c=state.config;
  for(const el of form.elements){if(!el.name||el.name=='password'||el.name=='admin_password'||el.name=='clear_password')continue;
    if(el.type=='checkbox')el.checked=!!c[el.name];else el.value=c[el.name]}
  form.password.value='';form.admin_password.value='';form.clear_password.checked=false;
  $('#pwstate').textContent=c.has_password?'(set)':'(none — open server)';
  $('#apwstate').textContent=c.has_admin_password?'(set)':'(none)';dlg.showModal()};
$('#setcancel').onclick=()=>dlg.close();
function settings(){const o={};
  for(const el of form.elements){if(!el.name)continue;
    if(el.type=='checkbox')o[el.name]=el.checked;else if(el.type=='number')o[el.name]=parseInt(el.value||'0',10);else o[el.name]=el.value}
  return o}
async function save(restart){try{await api('settings',settings());dlg.close();
  if(restart){say('Restarting …');try{await api('restart',{})}catch(_){}
    for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,500));try{await refresh();say('Restarted.','okc');return}catch(_){}}
    say('Server did not come back; check the terminal.','err')}
  else{say('Settings saved.','okc');await refresh()}}catch(e){say(e.message,'err')}}
form.onsubmit=e=>{e.preventDefault();save(false)};$('#setrestart').onclick=()=>save(true);
refresh().catch(e=>say(e.message,'err'));setInterval(()=>{if(!dlg.open&&$('#prog').hidden)refresh().catch(()=>{})},10000);
</script></html>
"""


class AdminHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "dlrd-admin"
    sys_version = ""
    timeout = 120
    app = None                                       # the MasterApp

    def log_message(self, fmt, *args):
        pass

    def _reply(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        if ctype.startswith("text/html"):
            self.send_header("Content-Security-Policy",
                             "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'")
        self.end_headers()
        self.wfile.write(data)

    def _fail(self, code, msg):
        self._reply(code, {"error": msg})

    def _allowed(self):
        cfg = self.app.config.get()
        if cfg["admin_password"]:
            auth = self.headers.get("Authorization", "")
            ok = False
            if auth[:6].lower() == "basic ":
                try:
                    ok = check_admin_password(cfg["admin_password"],
                                              base64.b64decode(auth[6:]).decode("utf-8", "replace").partition(":")[2])
                except (binascii.Error, ValueError):
                    pass
            if not ok:
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="dlrd admin"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return False
        else:
            # No password: loopback only, and only for the loopback names - so a
            # web page on another origin cannot reach this through DNS rebinding.
            raw = (self.headers.get("Host") or "").strip()
            host = raw.split("]")[0] + "]" if raw.startswith("[") else raw.rsplit(":", 1)[0] if ":" in raw else raw
            if host not in LOOPBACK_HOSTS:
                self._fail(403, "admin window without a password only answers on localhost")
                return False
        return True

    def do_GET(self):
        if not self._allowed():
            return
        path = urllib.parse.urlsplit(self.path).path
        if path == "/":
            return self._reply(200, ADMIN_HTML.encode(), "text/html; charset=utf-8")
        if path == "/admin/api/state":
            if self.headers.get("X-DLR-Admin") != "1":
                return self._fail(403, "missing X-DLR-Admin header")
            return self._reply(200, self.app.state())
        self._fail(404, "not found")

    def do_POST(self):
        if not self._allowed():
            return
        if self.headers.get("X-DLR-Admin") != "1":     # a cross-site form cannot set this
            return self._fail(403, "missing X-DLR-Admin header")
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        try:
            if url.path == "/admin/api/upload":
                return self._upload(query)
            length = int(self.headers.get("Content-Length") or 0)
            if length > 1 << 20:
                return self._fail(413, "request too large")
            body = json.loads(self.rfile.read(length) or b"{}")
            result = self.app.action(url.path.rsplit("/", 1)[-1], body)
            self._reply(200, result)
            if url.path.endswith("/restart"):
                self.app.restart_requested = True
        except AdminError as e:
            self._fail(e.code, str(e))
        except (ValueError, KeyError) as e:
            self._fail(400, "bad request: %s" % e)

    def _upload(self, query):
        pkg = (query.get("pkg") or [""])[0]
        rel = (query.get("path") or [""])[0]
        if not NAME_RE.match(pkg) or not valid_rel_path(rel):
            return self._fail(400, "invalid package or file name: %r" % rel)
        length = self.headers.get("Content-Length")
        if length is None:
            return self._fail(411, "Content-Length required")
        length = int(length)
        limit = self.app.config.get()["max_upload_mb"] * 1024 * 1024
        if length > limit:
            return self._fail(413, "file is larger than the %d MiB upload limit" % (limit >> 20))
        stage = self.app.stage_dir(pkg)
        if not os.path.isdir(stage):
            return self._fail(409, "upload not started")
        dest = os.path.join(stage, *rel.split("/"))
        if os.path.commonpath([stage, os.path.abspath(dest)]) != stage:
            return self._fail(400, "invalid path")
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        remaining = length
        with open(dest, "wb") as f:
            while remaining:
                block = self.rfile.read(min(1 << 20, remaining))
                if not block:
                    return self._fail(400, "upload cut short")
                f.write(block)
                remaining -= len(block)
        self._reply(200, {"ok": True})


class AdminError(Exception):
    def __init__(self, msg, code=400):
        super().__init__(msg)
        self.code = code


class MasterApp:
    def __init__(self, config, store):
        self.config, self.store = config, store
        self.worker_count = 0
        self.restart_requested = False

    def stage_dir(self, pkg):
        return os.path.join(self.store.staging, pkg)

    def state(self):
        cfg = dict(self.config.get())
        cfg["has_password"] = bool(cfg.pop("password_hash"))
        cfg["has_admin_password"] = bool(cfg.pop("admin_password"))
        packages = [{k: m[k] for k in ("name", "version", "description", "arch", "os", "size",
                                       "file_count", "files")} for m in self.store.list()]
        return {"config": cfg, "packages": packages, "workers": self.worker_count}

    def action(self, name, body):
        store = self.store
        pkg = str(body.get("pkg", ""))
        if name in ("begin", "finalize", "abort", "delete", "rebuild") and not NAME_RE.match(pkg):
            raise AdminError("invalid package name")
        if name == "begin":
            if os.path.isdir(store.pkg_dir(pkg)) and not body.get("replace"):
                raise AdminError("package '%s' already exists (tick 'replace if it exists')" % pkg, 409)
            shutil.rmtree(self.stage_dir(pkg), ignore_errors=True)
            os.makedirs(self.stage_dir(pkg))
            return {"ok": True}
        if name == "abort":
            shutil.rmtree(self.stage_dir(pkg), ignore_errors=True)
            return {"ok": True}
        if name == "finalize":
            stage = self.stage_dir(pkg)
            if not os.path.isdir(stage):
                raise AdminError("upload not started", 409)
            if os.path.isdir(store.pkg_dir(pkg)):
                if not body.get("replace"):
                    raise AdminError("package '%s' already exists" % pkg, 409)
                shutil.rmtree(store.pkg_dir(pkg))
            os.replace(stage, store.pkg_dir(pkg))
            try:
                store.build(pkg)
            except (OSError, ValueError, tarfile.TarError) as e:
                raise AdminError("could not build the archive: %s" % e, 500)
            log("published %s via admin" % pkg)
            return {"ok": True}
        if name == "delete":
            if not store.remove(pkg):
                raise AdminError("no such package", 404)
            log("deleted %s via admin" % pkg)
            return {"ok": True}
        if name == "rebuild":
            try:
                store.build(pkg)
            except (OSError, ValueError, tarfile.TarError) as e:
                raise AdminError(str(e), 404)
            return {"ok": True}
        if name == "settings":
            return self._settings(body)
        if name == "restart":
            return {"ok": True}
        raise AdminError("unknown action", 404)

    def _settings(self, body):
        cur = self.config.get()
        ch = {}
        for key in ("name", "cert_file", "key_file", "admin_host", "public_host", "udp_public"):
            if key in body:
                ch[key] = str(body[key])
        for key in ("dlr_port", "http_port", "https_port", "udp_port", "admin_port", "workers"):
            if key in body:
                ch[key] = int(body[key])
        for key in ("sniff_dlr_on_http", "broadcast"):
            if key in body:
                ch[key] = bool(body[key])
        if body.get("clear_password"):
            ch["password_hash"] = ""
        elif body.get("password"):
            ch["password_hash"] = sha256_hex(str(body["password"]))
        if body.get("admin_password"):
            ch["admin_password"] = hash_admin_password(str(body["admin_password"]))
        merged = dict(cur)
        merged.update(ch)
        if merged["https_port"] and not (merged["cert_file"] and merged["key_file"]):
            raise AdminError("HTTPS needs both the certificate and the key file")
        for key in ("cert_file", "key_file"):
            if merged[key] and not os.path.isfile(merged[key]):
                raise AdminError("%s does not exist: %s" % (key, merged[key]))
        if merged["admin_port"] and merged["admin_host"] not in ("127.0.0.1", "::1", "localhost") \
                and not merged["admin_password"]:
            raise AdminError("an admin password is required when the window is reachable from the network")
        self.config.update(ch)
        return {"ok": True}


def run_admin(app, cfg):
    host = cfg["admin_host"]
    loopback = host in ("127.0.0.1", "::1", "localhost")
    if not loopback and not cfg["admin_password"]:
        log("admin window disabled: binding %s needs an admin password (set admin_password in the "
            "config or bind it to 127.0.0.1)" % host)
        return None

    class Handler(AdminHandler):
        pass
    Handler.app = app
    ThreadingHTTPServer.daemon_threads = True
    ThreadingHTTPServer.allow_reuse_address = True
    try:
        srv = ThreadingHTTPServer((host, cfg["admin_port"]), Handler)
    except OSError as e:
        log("admin window cannot listen on %s:%d: %s" % (host, cfg["admin_port"], e))
        return None
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log("admin window: http://%s:%d/" % ("127.0.0.1" if host in ("0.0.0.0", "::") else host, cfg["admin_port"]))
    return srv


# =============================================================================
# master
# =============================================================================
def broadcaster(config):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    while True:
        cfg = config.get()
        if cfg["broadcast"] and cfg["dlr_port"]:
            msg = "DLR|%s|%s|%d\n" % (cfg["name"], "1" if cfg["password_hash"] else "0", cfg["dlr_port"])
            try:
                sock.sendto(msg.encode(), ("255.255.255.255", DISCOVERY_PORT))
            except OSError:
                pass
        time.sleep(5)


def main():
    ap = argparse.ArgumentParser(description="Deliver server for Minimal-OS (Linux/Python)")
    ap.add_argument("--data", default=os.environ.get("DLRD_DATA", "data"),
                    help="data directory (config.json, packages/); default ./data")
    ap.add_argument("--workers", type=int, help="worker processes (default: one per CPU core)")
    ap.add_argument("--name", help="server name (saved)")
    ap.add_argument("--password", help="client password (saved as a hash); '' removes it")
    ap.add_argument("--dlr-port", type=int, help="native protocol port (saved)")
    ap.add_argument("--http-port", type=int, help="web/API port (saved)")
    ap.add_argument("--https-port", type=int, help="HTTPS port (saved)")
    ap.add_argument("--udp-port", type=int, help="dlr-over-UDP port, 0 disables (saved)")
    ap.add_argument("--udp-public", help="host:port the UDP side is reachable at, e.g. a playit.gg address (saved)")
    ap.add_argument("--cert", help="TLS certificate chain (saved)")
    ap.add_argument("--key", help="TLS private key (saved)")
    ap.add_argument("--admin-port", type=int, help="admin window port, 0 disables (saved)")
    ap.add_argument("--open", action="store_true", help="open the admin window in a browser")
    args = ap.parse_args()

    os.makedirs(args.data, exist_ok=True)
    config = Config(os.path.join(args.data, "config.json"))
    changes = {}
    for opt, key in (("name", "name"), ("dlr_port", "dlr_port"), ("http_port", "http_port"),
                     ("https_port", "https_port"), ("udp_port", "udp_port"), ("udp_public", "udp_public"), ("cert", "cert_file"), ("key", "key_file"),
                     ("admin_port", "admin_port"), ("workers", "workers")):
        if getattr(args, opt) is not None:
            changes[key] = getattr(args, opt)
    if args.password is not None:
        changes["password_hash"] = sha256_hex(args.password) if args.password else ""
    if changes or not os.path.exists(config.path):
        config.update(changes)
    cfg = config.get()

    store = Store(args.data)
    store.sync()
    app = MasterApp(config, store)

    listeners = []
    for kind, port, what in (("dlr", cfg["dlr_port"], "dlr clients"),
                             ("udp", cfg["udp_port"], "dlr over UDP"),
                             ("http", cfg["http_port"], "the website"),
                             ("https", cfg["https_port"], "HTTPS")):
        if not port:
            continue
        if kind == "https" and not (cfg["cert_file"] and cfg["key_file"]):
            log("HTTPS port set but cert_file/key_file are not: HTTPS is off")
            continue
        sock = bind_udp(port, what) if kind == "udp" else bind(port, what)
        if sock:
            listeners.append((kind, sock))
            log("listening on %s port %d (%s)" % ("UDP" if kind == "udp" else "TCP", port, what))
    if not listeners:
        log("nothing to listen on - fix the ports above (admin window still starts)")

    nworkers = args.workers or cfg["workers"] or os.cpu_count() or 1
    mp = multiprocessing.get_context("fork")
    workers = []

    def spawn():
        p = mp.Process(target=worker_main, args=(listeners, args.data, config.path), daemon=True)
        p.start()
        return p

    # Fork before starting any thread of our own.
    if listeners:
        workers = [spawn() for _ in range(nworkers)]
    app.worker_count = len(workers)
    log("%s: %d worker process%s, data in %s" % (cfg["name"], len(workers), "" if len(workers) == 1 else "es",
                                                os.path.abspath(args.data)))

    threading.Thread(target=broadcaster, args=(config,), daemon=True).start()
    admin = run_admin(app, cfg) if cfg["admin_port"] else None
    if args.open and admin:
        import webbrowser
        webbrowser.open("http://127.0.0.1:%d/" % cfg["admin_port"])

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())

    tick = 0
    while not stop.wait(1.0) and not app.restart_requested:
        tick += 1
        for i, p in enumerate(workers):
            if not p.is_alive():
                log("worker %d died (exit %s), restarting" % (p.pid, p.exitcode))
                workers[i] = spawn()
        if tick % 3 == 0:                              # pick up folders dropped straight into packages/
            store.sync()

    log("restarting" if app.restart_requested else "shutting down")
    if admin:
        admin.shutdown()
    for p in workers:
        p.terminate()
    for p in workers:
        p.join(2)
    for _, s in listeners:
        s.close()
    if app.restart_requested:
        time.sleep(0.3)
        os.execv(sys.executable, [sys.executable] + sys.argv)


if __name__ == "__main__":
    main()
