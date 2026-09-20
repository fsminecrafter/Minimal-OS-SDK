#!/bin/sh
# End-to-end test of the Deliver server on Minimal-OS's port layer, using
# the host backend (real sockets, real files under a temp root).
#
#   MOS_KERNEL=/path/to/Minimal-OS ./e2e.sh              # Minimal-OS client <-> Minimal-OS server
#   DELIVER_BUILD=/path/to/deliver/build ./e2e.sh        # ...plus the C++ `dlr` client
#
# MOS_KERNEL is needed for tools/mkpkg/mkpkg.py (the host stand-in for the
# kernel's mos_pkg_zip). DELIVER_BUILD is optional: a CMake build dir that
# contains the C++ `dlr` binary; those checks run only if it is given and
# port 4242 (the port the C++ client probes) is free.
#
# What this does NOT test: anything that only exists on the real kernel -
# process spawning, handle ownership, the NIC. See tests/net-host in the
# kernel repo for the TCP/syscall layer.
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
KERNEL=${MOS_KERNEL:-"$HERE/../../../Minimal-OS"}
[ -f "$KERNEL/tools/mkpkg/mkpkg.py" ] || { echo "set MOS_KERNEL to the Minimal-OS repo" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

"$HERE/build.sh" >/dev/null || { echo "host build failed" >&2; exit 2; }
H="$HERE/dlr-host"
export DLR_MKPKG="$KERNEL/tools/mkpkg/mkpkg.py"

T=$(mktemp -d)
PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null; done; rm -rf "$T"; }
trap cleanup EXIT INT TERM

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
check() { if [ "$1" = 0 ]; then ok; else bad "$2"; fi; }
section() { echo "$1"; }

# Pick two free ports.
free_port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
P1=$(free_port); P2=$(free_port)

# ---- a package with every kind of content ---------------------------------
SRV="$T/srv"; SRC="$SRV/src/hello-mos"
mkdir -p "$SRC/notes"
cat > "$SRC/hello-mos.pkg" <<'PKG'
[Info]
name=hello-mos
version=1.2
description=Minimal-OS end to end test package
arch=x86_64
operatingsystem=MINIMALOS

[Install.minimalos]
copy=hello.run,notes/big.txt,notes/empty.txt,notes/random.bin
target=0:/apps/hello-mos
PKG
python3 - "$SRC" <<'PY'
import os, sys, random
r = sys.argv[1]; random.seed(5)
open(f"{r}/hello.run", "wb").write(bytes(random.getrandbits(8) for _ in range(30000)))
open(f"{r}/notes/big.txt", "w").write(("The quick brown fox jumps over the lazy dog. " * 40 + "\n") * 120)
open(f"{r}/notes/empty.txt", "wb").close()
open(f"{r}/notes/random.bin", "wb").write(os.urandom(70000))
PY

same_tree() {   # $1 = installed dir
    for f in hello.run notes/big.txt notes/empty.txt notes/random.bin; do
        cmp -s "$SRC/$f" "$1/$f" || return 1
    done
}

section "presenting a package"
DLR_ROOT="$SRV" "$H" present 0:/src/hello-mos >/dev/null 2>&1; check $? "present"
[ -f "$SRV/var/dlrd/packages/hello-mos.mpkg" ]; check $? "present built the .mpkg"
DLR_ROOT="$SRV" "$H" present 0:/src/hello-mos >/dev/null 2>&1; [ $? -ne 0 ]; check $? "presenting the same name twice is refused"
DLR_ROOT="$SRV" "$H" packages 2>&1 | grep -q "hello-mos.*1.2"; check $? "packages lists it with its version"

start_server() {   # port, extra args...
    port=$1; shift
    DLR_ROOT="$SRV" "$H" serve --port "$port" "$@" >"$T/server-$port.log" 2>&1 &
    PIDS="$PIDS $!"; SERVER_PID=$!
    # wait until it accepts
    i=0; while [ $i -lt 50 ]; do
        python3 -c "import socket,sys; socket.create_connection(('127.0.0.1',$port),timeout=1).close()" 2>/dev/null && return 0
        sleep 0.1; i=$((i+1))
    done
    echo "server on $port did not start:"; cat "$T/server-$port.log"; return 1
}
start_server "$P1" --name e2e || exit 2
start_server "$P2" --name locked --password s3cret || exit 2

client() { root=$1; shift; DLR_ROOT="$T/$root" timeout 60 "$H" "$@" 2>&1; }

section "client against the open server"
client c1 add 127.0.0.1:$P1 | grep -q "Added 'e2e'"; check $? "add"
client c1 list | grep -q "hello-mos.*1.2"; check $? "list"
client c1 search hello | grep -q "hello-mos"; check $? "search hit"
client c1 search zzz-no-such | grep -qi "hello-mos"; [ $? -ne 0 ]; check $? "search miss returns nothing"
client c1 ping | grep -qi "pong\|ms"; check $? "ping"
client c1 install hello-mos | grep -q "Installed 4 file"; check $? "install"
same_tree "$T/c1/apps/hello-mos"; check $? "installed files are byte-identical (stored, LZSS and empty)"
client c1 install no-such-pkg >/dev/null; [ $? -ne 0 ]; check $? "installing an unknown package fails"
client c1 install '../etc' >/dev/null; [ $? -ne 0 ]; check $? "path-traversal name is refused"

section "six clients at once (server allows 3 at a time)"
KIDS=""
for i in 1 2 3 4 5 6; do
    ( client k$i add 127.0.0.1:$P1 >/dev/null; client k$i install hello-mos >"$T/k$i.out"; echo $? >"$T/k$i.rc" ) &
    KIDS="$KIDS $!"
done
# Not a bare `wait`: that would also wait for the servers.
for k in $KIDS; do wait "$k"; done
n=0; for i in 1 2 3 4 5 6; do
    [ "$(cat "$T/k$i.rc" 2>/dev/null)" = 0 ] && same_tree "$T/k$i/apps/hello-mos" && n=$((n+1))
done
[ $n = 6 ]; check $? "all six installs succeeded and are identical ($n/6)"

section "password"
client p1 add 127.0.0.1:$P2 | grep -q "password required"; check $? "add notices the password"
client p1 list >/dev/null;                       [ $? -ne 0 ]; check $? "no password is refused"
client p1 list --password nope >/dev/null;       [ $? -ne 0 ]; check $? "wrong password is refused"
client p1 list --password s3cret | grep -q hello-mos; check $? "right password works"
client p1 install hello-mos --password s3cret | grep -q "Installed 4 file"; check $? "install with password"

section "hostile connections"
python3 - "$P1" <<'PY'
import socket, struct, sys, time, base64
port = int(sys.argv[1]); fails = 0
def conn():  return socket.create_connection(("127.0.0.1", port), timeout=15)
def frame(s):
    h = b""
    while len(h) < 4:
        c = s.recv(4 - len(h))
        if not c: return None
        h += c
    n = struct.unpack(">I", h)[0]; d = b""
    while len(d) < n:
        c = s.recv(n - len(d))
        if not c: break
        d += c
    return d
def expect(label, cond):
    global fails
    print(("  ok   " if cond else "  FAIL ") + label)
    fails += 0 if cond else 1

s = conn(); h = frame(s); s.close()
expect("hello frame is DLR_SERVER|name|pw|1", h is not None and h.startswith(b"DLR_SERVER|e2e|0|"))

s = conn(); frame(s); s.send(struct.pack(">I", 7) + b"garbage"); expect("garbage instead of KEY -> dropped", s.recv(8) == b""); s.close()
s = conn(); frame(s); s.send(struct.pack(">I", 0xFFFFFFFF));   expect("4 GiB frame length -> dropped",       s.recv(8) == b""); s.close()
k = base64.b64encode(b"short")
s = conn(); frame(s); s.send(struct.pack(">I", 4 + len(k)) + b"KEY:" + k); expect("KEY of the wrong length -> dropped", s.recv(8) == b""); s.close()

t = time.time(); s = conn(); frame(s); s.settimeout(15)
closed = s.recv(8) == b""; dt = time.time() - t; s.close()
expect("silent client dropped after the KEY timeout (%.1fs)" % dt, closed and 3 <= dt <= 8)

for _ in range(8):                       # probe-and-leave, like `dlr scan`
    s = conn(); frame(s); s.close()
expect("connect-read-hello-close x8 accepted", True)
sys.exit(1 if fails else 0)
PY
check $? "hostile-connection checks"
client c1 list | grep -q hello-mos; check $? "server still serves normally afterwards"

section "shutdown"
kill -TERM "$SERVER_PID" 2>/dev/null   # the locked server (last started)
sleep 1
python3 -c "import socket; socket.create_connection(('127.0.0.1',$P2),timeout=1)" 2>/dev/null; [ $? -ne 0 ]
check $? "SIGTERM stops the server and closes its port"
grep -q "stopping" "$T/server-$P2.log"; check $? "it said so"

# ---- the C++ client, if we have one ----------------------------------------
if [ -n "${DELIVER_BUILD:-}" ] && [ -x "$DELIVER_BUILD/dlr" ]; then
    if python3 -c "import socket; s=socket.socket(); s.bind(('0.0.0.0',4242))" 2>/dev/null; then
        section "C++ Deliver client (warns on .mpkg)"
        start_server 4242 --name cpp42 || exit 2
        export HOME="$T/home"; mkdir -p "$HOME"; D="$DELIVER_BUILD/dlr"
        timeout 60 "$D" search hello >/dev/null 2>&1
        out=$(timeout 60 "$D" install -y hello-mos 2>&1); rc=$?
        [ $rc -ne 0 ]; check $? "install is refused (rc=$rc)"
        echo "$out" | grep -q "cannot extract .mpkg"; check $? "install says why"
        echo "$out" | grep -q "dlr download hello-mos"; check $? "install points to 'dlr download'"
        mkdir -p "$T/dl"; ( cd "$T/dl" && timeout 60 "$D" download -y hello-mos >"$T/dl.out" 2>&1 ); rc=$?
        check $rc "download succeeds"
        cmp -s "$T/dl/hello-mos.mpkg" "$SRV/var/dlrd/packages/hello-mos.mpkg"; check $? "download keeps the exact .mpkg the server has"
        grep -q "Saved, but not extracted" "$T/dl.out"; check $? "download says it was not extracted"
    else
        echo "(skipping C++ client checks: port 4242 is in use)"
    fi
else
    echo "(skipping C++ client checks: set DELIVER_BUILD to a Deliver build dir)"
fi

echo
echo "$PASS checks passed, $FAIL failed"
[ "$FAIL" = 0 ]
