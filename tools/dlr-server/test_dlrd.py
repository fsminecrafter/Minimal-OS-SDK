#!/usr/bin/env python3
"""
Interop test for dlrd. Builds the real Minimal-OS dlr client against the host
backend (tests/dlr-host) and runs it against a live dlrd, then pokes the HTTP
side and the admin window.

    python3 test_dlrd.py
"""
import base64
import filecmp
from http.client import HTTPConnection
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import threading
import unittest
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.abspath(os.path.join(HERE, "..", ".."))
HOST_CLIENT = os.path.join(SDK, "tests", "dlr-host", "dlr-host")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def http(port, path, headers=None, method="GET", body=None):
    c = HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, dict(r.getheaders()), data


class Server:
    def __init__(self, tmp, password=None, extra=()):
        self.data = os.path.join(tmp, "data")
        self.ports = dict(dlr=free_port(), web=free_port(), admin=free_port(), udp=free_port())
        os.makedirs(os.path.join(self.data, "packages", "hello-mos", "notes"))
        root = os.path.join(self.data, "packages", "hello-mos")
        with open(os.path.join(root, "hello-mos.pkg"), "w") as f:
            f.write("[Info]\nname=hello-mos\nversion=1.2\ndescription=Interop test package\n"
                    "arch=x86_64\noperatingsystem=MINIMALOS\n\n[Install.minimalos]\n"
                    "copy=hello.run,notes/big.txt,notes/empty.txt,notes/random.bin\ntarget=0:/apps/hello-mos\n")
        with open(os.path.join(root, "hello.run"), "wb") as f:
            f.write(os.urandom(30000))
        with open(os.path.join(root, "notes", "big.txt"), "w") as f:
            f.write("The quick brown fox. " * 5000)
        open(os.path.join(root, "notes", "empty.txt"), "wb").close()
        with open(os.path.join(root, "notes", "random.bin"), "wb") as f:
            f.write(os.urandom(70000))
        self.src = root
        cmd = [sys.executable, os.path.join(HERE, "dlrd.py"), "--data", self.data,
               "--dlr-port", str(self.ports["dlr"]), "--http-port", str(self.ports["web"]),
               "--admin-port", str(self.ports["admin"]), "--udp-port", str(self.ports["udp"]),
               "--udp-public", "127.0.0.1:%d" % self.ports["udp"], "--workers", "3", "--name", "testsrv", *extra]
        if password is not None:
            cmd += ["--password", password]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.ports["admin"]), timeout=0.5).close()
                break
            except OSError:
                time.sleep(0.1)
        else:
            self.stop()
            raise RuntimeError("dlrd did not start:\n" + self.proc.stdout.read())

    def stop(self):
        self.proc.terminate()
        try:
            self.out = self.proc.communicate(timeout=10)[0]
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.out = self.proc.communicate()[0]


def client(root, *args):
    env = dict(os.environ, DLR_ROOT=root)
    r = subprocess.run([HOST_CLIENT, *args], env=env, capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout + r.stderr


import hashlib
import hmac
import struct

MAGIC = b"DLU1"


class UdpClient:
    """Reference client for the DLU1 protocol (what the C client mirrors)."""

    def __init__(self, port, password=None, drop=0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.25)
        self.addr = ("127.0.0.1", port)
        self.key = hashlib.sha256(password.encode()).digest() if password is not None else None
        self.drop, self.n = drop, 0
        self.rng = __import__("random").Random(7)
        r = self.rpc(MAGIC + b"\x01")
        assert r[4] == 0x81, r
        self.cookie, self.flags, self.name = r[5:21], r[21], r[22:].decode()

    def rpc(self, datagram, want=1):
        out = []
        for _ in range(5):
            self.sock.sendto(datagram, self.addr)
            try:
                while len(out) < want:
                    out.append(self.sock.recvfrom(2048)[0])
                return out[0] if want == 1 else out
            except socket.timeout:
                continue
        raise TimeoutError

    def req(self, op, body=b"", want=1):
        proof = (hmac.new(self.key, self.cookie + bytes([op]) + body, hashlib.sha256).digest()[:16]
                 if self.key else bytes(16))
        return self.rpc(MAGIC + bytes([op]) + self.cookie + proof + body, want)

    def download(self, name):
        info = self.req(5, name.encode())
        assert info[4] == 0x85, info
        size = struct.unpack(">Q", info[5:13])[0]
        sha = info[13:45].hex()
        data, total, window = {}, (size + 999) // 1000, 8
        for _ in range(total * 4 + 20):                      # bounded: a test must not hang
            missing = [i for i in range(total) if i not in data]
            if not missing:
                break
            for off, chunk in self.sock_get(name, missing[0] * 1000, window):
                data[off // 1000] = chunk
        assert len(data) >= total, "download did not finish"
        blob = b"".join(data[i] for i in sorted(data))[:size]
        return blob, sha

    def sock_get(self, name, offset, n):
        body = bytes([len(name)]) + name.encode() + struct.pack(">QB", offset, n)
        proof = (hmac.new(self.key, self.cookie + b"\x06" + body, hashlib.sha256).digest()[:16]
                 if self.key else bytes(16))
        pkt = MAGIC + b"\x06" + self.cookie + proof + body
        self.sock.sendto(pkt, self.addr)
        out = {}
        end = time.time() + 0.25
        while time.time() < end and len(out) < n:
            try:
                r = self.sock.recvfrom(2048)[0]
            except socket.timeout:
                break
            if r[4] != 0x86:
                raise AssertionError(r)
            self.n += 1
            if self.drop and self.rng.random() < 1.0 / self.drop:   # simulate loss
                continue
            off = struct.unpack(">Q", r[5:13])[0]
            out[off] = r[13:]
        return sorted(out.items())


class LossyProxy(threading.Thread):
    """UDP relay that drops a share of the server's replies, like a bad link."""

    def __init__(self, target_port, loss=0.2):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.port = self.sock.getsockname()[1]
        self.target, self.loss = ("127.0.0.1", target_port), loss
        self.rng = __import__("random").Random(3)
        self.up = {}                                   # client addr -> upstream socket
        self.start()

    def run(self):
        while True:
            data, addr = self.sock.recvfrom(4096)
            if addr not in self.up:
                u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                self.up[addr] = u
                threading.Thread(target=self.back, args=(u, addr), daemon=True).start()
            self.up[addr].sendto(data, self.target)

    def back(self, u, addr):
        while True:
            data, _ = u.recvfrom(4096)
            if self.rng.random() >= self.loss:
                self.sock.sendto(data, addr)


class Interop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["sh", os.path.join(SDK, "tests", "dlr-host", "build.sh")], check=True,
                       capture_output=True)
        cls.tmp = tempfile.TemporaryDirectory()
        cls.srv = Server(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.tmp.cleanup()

    def test_1_published(self):
        status, _, body = http(self.srv.ports["web"], "/api/package")
        self.assertEqual(status, 200)
        names = [p["name"] for p in json.loads(body)["packages"]]
        self.assertEqual(names, ["hello-mos"])

    def test_2_real_client(self):
        root = os.path.join(self.tmp.name, "c1")
        os.makedirs(root)
        rc, out = client(root, "add", "127.0.0.1:%d" % self.srv.ports["dlr"])
        self.assertIn("Added 'testsrv'", out, out)
        rc, out = client(root, "list")
        self.assertRegex(out, r"hello-mos.*1\.2", out)
        rc, out = client(root, "search", "interop")
        self.assertIn("hello-mos", out)
        rc, out = client(root, "search", "zzz-nothing")
        self.assertNotIn("hello-mos", out)
        rc, out = client(root, "ping")
        self.assertEqual(rc, 0, out)
        rc, out = client(root, "install", "hello-mos")
        self.assertEqual(rc, 0, out)
        self.assertIn("Installed 4 file", out)
        for f in ("hello.run", "notes/big.txt", "notes/empty.txt", "notes/random.bin"):
            self.assertTrue(filecmp.cmp(os.path.join(self.srv.src, f),
                                        os.path.join(root, "apps", "hello-mos", f), shallow=False), f)
        rc, out = client(root, "install", "no-such-pkg")
        self.assertNotEqual(rc, 0)
        rc, out = client(root, "install", "../etc")
        self.assertNotEqual(rc, 0)

    def test_3_parallel_clients(self):
        procs = []
        for i in range(8):
            root = os.path.join(self.tmp.name, "k%d" % i)
            os.makedirs(root)
            client(root, "add", "127.0.0.1:%d" % self.srv.ports["dlr"])
            procs.append((root, subprocess.Popen([HOST_CLIENT, "install", "hello-mos"],
                                                 env=dict(os.environ, DLR_ROOT=root),
                                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)))
        for root, p in procs:
            p.communicate(timeout=60)
            self.assertEqual(p.returncode, 0)
            self.assertTrue(os.path.exists(os.path.join(root, "apps", "hello-mos", "hello.run")))

    def test_4_pages_and_api(self):
        port = self.srv.ports["web"]
        status, _, body = http(port, "/")
        self.assertEqual(status, 200)
        self.assertIn(b"testsrv", body)
        status, hdr, body = http(port, "/list")
        self.assertEqual(status, 200)
        self.assertIn(b"hello-mos", body)
        self.assertIn(b"big.txt", body)
        self.assertIn(b"Content-Security-Policy", b"".join(k.encode() for k in hdr))
        status, hdr, tar = http(port, "/api/package/hello-mos")
        self.assertEqual(status, 200)
        self.assertEqual(hdr["Content-Type"], "application/x-tar")
        import hashlib
        self.assertEqual(hashlib.sha256(tar).hexdigest(), hdr["X-DLR-SHA256"])
        status, _, body = http(port, "/api/package/hello-mos/info")
        self.assertEqual(json.loads(body)["file_count"], 5)     # 4 files + the manifest
        self.assertEqual(http(port, "/api/package/nope")[0], 404)
        self.assertEqual(http(port, "/api/package/..%2f..%2fetc%2fpasswd")[0], 404)
        self.assertEqual(http(port, "/api/package", method="POST", body=b"x")[0], 405)
        status, _, body = http(port, "/api/package?q=zzz")
        self.assertEqual(json.loads(body)["packages"], [])

    def test_5_admin_upload(self):
        port = self.srv.ports["admin"]
        H = {"X-DLR-Admin": "1"}
        self.assertEqual(http(port, "/admin/api/state")[0], 403)             # no header
        self.assertEqual(http(port, "/admin/api/state", {"Host": "evil.example", **H})[0], 403)
        self.assertEqual(http(port, "/admin/api/begin", H, "POST", b'{"pkg":"x"}')[0], 200)
        up = lambda rel: http(port, "/admin/api/upload?pkg=x&path=" + rel, H, "POST", b"hi")[0]
        self.assertEqual(up("a/b.txt"), 200)
        self.assertEqual(up("..%2Fevil"), 400)
        self.assertEqual(up("%2Fabs"), 400)
        self.assertEqual(up("a%5Cb"), 400)
        self.assertEqual(http(port, "/admin/api/finalize", {**H, "Content-Type": "application/json"},
                              "POST", b'{"pkg":"x"}')[0], 200)
        _, _, body = http(self.srv.ports["web"], "/api/package")
        self.assertIn("x", [p["name"] for p in json.loads(body)["packages"]])
        self.assertTrue(os.path.exists(os.path.join(self.srv.data, "packages", "x", "x.pkg")))  # manifest made
        self.assertEqual(http(port, "/admin/api/begin", H, "POST", b'{"pkg":"x"}')[0], 409)  # exists
        self.assertEqual(http(port, "/admin/api/delete", H, "POST", b'{"pkg":"x"}')[0], 200)

    def test_7_udp(self):
        c = UdpClient(self.srv.ports["udp"])
        self.assertEqual(c.name, "testsrv")
        self.assertEqual(c.flags, 0)
        r = c.req(2, b"hello")
        self.assertEqual(r[4:], b"\x82hello")
        r = c.req(3, b"\x00\x00")
        self.assertEqual(r[4], 0x83)
        self.assertIn(b"hello-mos|1.2|Interop test package", r[9:])
        r = c.req(4, b"\x00\x00zzz-nothing")
        self.assertEqual(r[9:], b"NONE")
        self.assertEqual(c.req(5, b"nope")[4], 0xFF)
        blob, sha = c.download("hello-mos")
        self.assertEqual(hashlib.sha256(blob).hexdigest(), sha)
        _, hdr, tar = http(self.srv.ports["web"], "/api/package/hello-mos")
        self.assertEqual(blob, tar)                       # same archive as TCP and HTTP
        lossy = UdpClient(self.srv.ports["udp"], drop=4)  # every 4th chunk lost, client re-asks
        blob2, sha2 = lossy.download("hello-mos")
        self.assertEqual(blob2, tar)

    def same_tree(self, root):
        for f in ("hello.run", "notes/big.txt", "notes/empty.txt", "notes/random.bin"):
            self.assertTrue(filecmp.cmp(os.path.join(self.srv.src, f),
                                        os.path.join(root, "apps", "hello-mos", f), shallow=False), f)

    def test_7b_c_client_over_udp(self):
        root = os.path.join(self.tmp.name, "u1")
        os.makedirs(root)
        rc, out = client(root, "add", "udp://127.0.0.1:%d" % self.srv.ports["udp"])
        self.assertIn("over UDP", out, out)
        self.assertRegex(client(root, "list")[1], r"hello-mos.*1\.2")
        self.assertIn("hello-mos", client(root, "search", "interop")[1])
        self.assertNotIn("hello-mos", client(root, "search", "zzz-nothing")[1])
        self.assertEqual(client(root, "ping")[0], 0)
        rc, out = client(root, "install", "hello-mos")
        self.assertEqual(rc, 0, out)
        self.assertIn("Installed 4 file", out)
        self.same_tree(root)
        self.assertNotEqual(client(root, "install", "no-such-pkg")[0], 0)

    def test_7c_c_client_over_lossy_udp(self):
        proxy = LossyProxy(self.srv.ports["udp"], loss=0.2)
        root = os.path.join(self.tmp.name, "u2")
        os.makedirs(root)
        rc, out = client(root, "add", "udp://127.0.0.1:%d" % proxy.port)
        self.assertIn("over UDP", out, out)
        rc, out = client(root, "install", "hello-mos")
        self.assertEqual(rc, 0, out)
        self.same_tree(root)

    def test_7d_http_redirects_to_udp(self):
        root = os.path.join(self.tmp.name, "u3")
        os.makedirs(root)
        rc, out = client(root, "add", "http://127.0.0.1:%d" % self.srv.ports["web"])
        self.assertIn("Redirected to the UDP stream", out, out)
        self.assertIn("over UDP", out, out)
        rc, out = client(root, "install", "hello-mos")
        self.assertEqual(rc, 0, out)
        self.same_tree(root)

    def test_8_udp_needs_cookie(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(1)
        s.sendto(MAGIC + b"\x03" + bytes(32) + b"\x00\x00", ("127.0.0.1", self.srv.ports["udp"]))
        self.assertEqual(s.recvfrom(2048)[0][4], 0xFF)    # stale cookie, tiny reply
        s.sendto(b"junk", ("127.0.0.1", self.srv.ports["udp"]))
        with self.assertRaises(socket.timeout):
            s.recvfrom(2048)

    def test_9_endpoint_and_links(self):
        status, hdr, body = http(self.srv.ports["web"], "/api/endpoint")
        info = json.loads(body)
        self.assertEqual(info["udp"], {"host": "127.0.0.1", "port": self.srv.ports["udp"]})
        self.assertEqual(hdr["X-DLR-UDP"], "127.0.0.1:%d" % self.srv.ports["udp"])
        _, _, page = http(self.srv.ports["web"], "/list")
        self.assertIn(b"<h3><a href='/api/package/hello-mos'", page)

    def test_6_drop_into_folder(self):
        d = os.path.join(self.srv.data, "packages", "dropped")
        os.makedirs(d)
        open(os.path.join(d, "f.txt"), "w").write("x")
        for _ in range(40):
            if http(self.srv.ports["web"], "/api/package/dropped/info")[0] == 200:
                break
            time.sleep(0.25)
        else:
            self.fail("folder dropped into packages/ was not published")


class Locked(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.srv = Server(cls.tmp.name, password="s3cret")

    @classmethod
    def tearDownClass(cls):
        cls.srv.stop()
        cls.tmp.cleanup()

    def test_c_client_udp_password(self):
        root = os.path.join(self.tmp.name, "up")
        os.makedirs(root)
        rc, out = client(root, "add", "udp://127.0.0.1:%d" % self.srv.ports["udp"])
        self.assertIn("password required", out)
        self.assertNotEqual(client(root, "list")[0], 0)
        self.assertNotEqual(client(root, "list", "--password", "nope")[0], 0)
        self.assertIn("hello-mos", client(root, "list", "--password", "s3cret")[1])
        rc, out = client(root, "install", "hello-mos", "--password", "s3cret")
        self.assertEqual(rc, 0, out)

    def test_udp_password(self):
        port = self.srv.ports["udp"]
        c = UdpClient(port, password="s3cret")
        self.assertEqual(c.flags, 1)
        self.assertEqual(c.req(2, b"x")[4], 0x82)
        blob, sha = c.download("hello-mos")
        self.assertEqual(hashlib.sha256(blob).hexdigest(), sha)
        bad = UdpClient(port, password="wrong")
        self.assertEqual(bad.req(3, b"\x00\x00")[4], 0xFF)
        self.assertEqual(UdpClient(port).req(3, b"\x00\x00")[4], 0xFF)   # no password

    def test_password(self):
        root = os.path.join(self.tmp.name, "p")
        os.makedirs(root)
        rc, out = client(root, "add", "127.0.0.1:%d" % self.srv.ports["dlr"])
        self.assertIn("password required", out)
        self.assertNotEqual(client(root, "list")[0], 0)
        self.assertNotEqual(client(root, "list", "--password", "nope")[0], 0)
        self.assertIn("hello-mos", client(root, "list", "--password", "s3cret")[1])
        rc, out = client(root, "install", "hello-mos", "--password", "s3cret")
        self.assertEqual(rc, 0, out)

    def test_http_password(self):
        port = self.srv.ports["web"]
        self.assertEqual(http(port, "/")[0], 200)                    # home page stays public
        self.assertEqual(http(port, "/api/package")[0], 401)
        self.assertEqual(http(port, "/list")[0], 401)
        self.assertEqual(http(port, "/api/package/hello-mos")[0], 401)
        self.assertEqual(http(port, "/api/package", {"X-DLR-Password": "s3cret"})[0], 200)
        basic = base64.b64encode(b"u:s3cret").decode()
        self.assertEqual(http(port, "/api/package/hello-mos", {"Authorization": "Basic " + basic})[0], 200)
        self.assertEqual(http(port, "/api/package", {"X-DLR-Password": "wrong"})[0], 401)


if __name__ == "__main__":
    unittest.main(verbosity=2)
