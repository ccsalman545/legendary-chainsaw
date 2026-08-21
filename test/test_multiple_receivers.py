#!/usr/bin/env python3
"""
Multi-receiver test for the legendary-chainsaw camera WebSocket server.

What it does
------------
1. Compiles a *test* server binary from the project's real sources
   (src/http_server.c, src/frame_stream.c, src/frame_queue.c,
   src/camera_worker.c, src/http_main.c, third_party/mongoose) linked
   against the test-only synthetic camera (test/camera_fake.c), so it
   runs anywhere with gcc and python3 -- no webcam needed.
2. Starts that server on a free port.
3. Opens several WebSocket receivers at once (raw stdlib clients) and
   verifies:
     - every receiver completes the 101 handshake,
     - every receiver receives video frames,
     - every receiver stays connected,
     - a late-joining receiver does NOT kick an already-streaming one
       (the exact regression that was just fixed: previously the server
       kept a single client and drained the previous one on connect).
4. Prints a report and exits non-zero on any failure.

Run it with:
    python3 test/test_multiple_receivers.py

Pure standard library (socket, hashlib, base64, struct, threading,
subprocess, tempfile). Requires gcc.
"""

import base64
import hashlib
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Colours for the report (disabled if not a tty).
TTY = sys.stdout.isatty()


def c(code, text):
    if not TTY:
        return text
    return "\033[%sm%s\033[0m" % (code, text)


GREEN = lambda t: c("32", t)
RED = lambda t: c("31", t)
YELLOW = lambda t: c("33", t)
BOLD = lambda t: c("1", t)


# --------------------------------------------------------------------------
# Build the synthetic-camera test server
# --------------------------------------------------------------------------

SOURCES = [
    "src/http_main.c",
    "src/http_server.c",
    "src/frame_stream.c",
    "src/frame_queue.c",
    "src/camera_worker.c",
    "test/camera_fake.c",
    "third_party/mongoose/mongoose.c",
]

CFLAGS = [
    "-Iinclude",
    "-Ithird_party/mongoose",
    "-std=c11",
    "-D_POSIX_C_SOURCE=200809L",
    "-D_DEFAULT_SOURCE",
    "-O2",
    # Quiet Mongoose's own logging so the test output stays readable.
    "-DMG_ENABLE_LOG=0",
    "-Wall",
    "-Wextra",
]


def build_server(out_path):
    """Compile the test server. Returns True on success."""
    srcs = [os.path.join(REPO_ROOT, s) for s in SOURCES]
    for s in srcs:
        if not os.path.exists(s):
            print(RED("ERROR") + " missing source: %s" % s)
            return False

    cmd = ["gcc"] + CFLAGS + srcs + ["-o", out_path, "-pthread"]
    print(BOLD("Building") + " test server...")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(RED("BUILD FAILED"))
        print(proc.stderr)
        return False
    if proc.stderr.strip():
        # Warnings only.
        print(YELLOW("compiler warnings:"))
        print(proc.stderr)
    print(GREEN("built") + " %s" % out_path)
    return True


# --------------------------------------------------------------------------
# Networking helpers
# --------------------------------------------------------------------------

def find_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for_status(host, port, timeout=10.0):
    """Poll GET /status until it returns 200, or time out."""
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1.0) as s:
                s.sendall(b"GET /status HTTP/1.0\r\nHost: x\r\n\r\n")
                data = b""
                while b"\r\n\r\n" not in data and len(data) < 4096:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                last = data.decode("latin-1")
                if " 200 " in last.split("\r\n", 1)[0]:
                    return True
        except OSError:
            pass
        time.sleep(0.2)
    print(RED("ERROR") + " server did not become ready. Last response:")
    print(last or "(no response)")
    return False


def ws_handshake(host, port, path="/ws", timeout=5.0):
    """
    Perform a WebSocket handshake. Returns (sock, leftover_bytes) on
    success, or (None, reason) on failure.
    """
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n" % (path, host, port, key)
    )
    try:
        s = socket.create_connection((host, port), timeout=timeout)
    except OSError as e:
        return None, "connect failed: %s" % e

    s.sendall(req.encode())

    buf = b""
    s.settimeout(timeout)
    while b"\r\n\r\n" not in buf:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            s.close()
            return None, "handshake timed out"
        if not chunk:
            s.close()
            return None, "connection closed during handshake"
        buf += chunk

    header, rest = buf.split(b"\r\n\r\n", 1)
    status = header.split(b"\r\n", 1)[0]
    if b" 101 " not in status:
        s.close()
        return None, "no 101 (got %r)" % status

    # Validate Sec-WebSocket-Accept.
    accept_expected = base64.b64encode(
        hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest()
    ).decode()
    accept_got = None
    for line in header.split(b"\r\n"):
        if line.lower().startswith(b"sec-websocket-accept:"):
            accept_got = line.split(b":", 1)[1].strip().decode()
    if accept_got != accept_expected:
        s.close()
        return None, "bad Sec-WebSocket-Accept"

    return s, rest


def parse_frames(data):
    """Parse zero or more WebSocket frames from a buffer.

    Returns (frames, consumed) where frames is a list of (opcode, payload).
    Server-to-client frames are unmasked.
    """
    out = []
    i = 0
    n = len(data)
    while i + 2 <= n:
        b1 = data[i]
        b2 = data[i + 1]
        opcode = b1 & 0x0F
        length = b2 & 0x7F
        j = i + 2
        if length == 126:
            if j + 2 > n:
                break
            length = struct.unpack(">H", data[j:j + 2])[0]
            j += 2
        elif length == 127:
            if j + 8 > n:
                break
            length = struct.unpack(">Q", data[j:j + 8])[0]
            j += 8
        if b2 & 0x80:  # masked
            j += 4
        if j + length > n:
            break
        out.append((opcode, data[j:j + length]))
        i = j + length
    return out, i


class Receiver:
    """One WebSocket receiver that records frames it gets."""

    def __init__(self, name, host, port):
        self.name = name
        self.host = host
        self.port = port
        self.connected = False
        self.error = None
        self.frame_count = 0
        self.max_sequence = -1
        self.stayed_open = False
        self._sock = None
        # Persistent parse buffer so a frame split across two read()
        # calls is reassembled instead of mis-parsed.
        self._buffer = b""

    def connect(self):
        s, rest = ws_handshake(self.host, self.port)
        if s is None:
            self.error = rest
            return False
        self._sock = s
        self._buffer = rest
        self.connected = True
        return True

    def read(self, duration):
        """
        Read frames for `duration` seconds. Safe to call more than once:
        unparsed bytes are kept in self._buffer between calls.
        """
        if self._sock is None:
            return
        self._sock.settimeout(0.5)
        deadline = time.time() + duration
        closed = False
        while time.time() < deadline and not closed:
            try:
                chunk = self._sock.recv(65536)
            except socket.timeout:
                # No data right now; keep waiting until the deadline.
                continue
            if chunk == b"":
                # Peer sent FIN.
                closed = True
                break
            self._buffer += chunk
            frames, consumed = parse_frames(self._buffer)
            self._buffer = self._buffer[consumed:]
            for opcode, payload in frames:
                if opcode == 0x2:  # binary
                    self.frame_count += 1
                    if len(payload) >= 28:
                        seq = struct.unpack("<I", payload[24:28])[0]
                        self.max_sequence = max(self.max_sequence, seq)
                elif opcode == 0x8:  # close
                    closed = True
        self.stayed_open = not closed

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None


# --------------------------------------------------------------------------
# Test scenarios
# --------------------------------------------------------------------------

def line(ok, text):
    mark = GREEN("PASS") if ok else RED("FAIL")
    print("  [%s] %s" % (mark, text))
    return ok


def test_simultaneous(host, port, n=4, duration=4.0, stagger=0.25):
    """N receivers connect (staggered); all must get frames and stay open."""
    print(BOLD("\nTest 1: %d simultaneous receivers" % n))
    receivers = [Receiver("R%d" % i, host, port) for i in range(n)]

    # Stagger connects a little, like real browsers opening at once.
    for r in receivers:
        if not r.connect():
            print("  " + RED("ERROR") + " %s could not connect: %s" % (r.name, r.error))
        time.sleep(stagger)

    threads = []
    for r in receivers:
        t = threading.Thread(target=r.read, args=(duration,))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()

    all_ok = True
    for r in receivers:
        ok = r.connected and r.frame_count > 0 and r.stayed_open
        all_ok &= ok
        print("  %s: connected=%s frames=%d last_seq=%s open=%s" % (
            r.name,
            GREEN("yes") if r.connected else RED("no"),
            r.frame_count,
            r.max_sequence,
            GREEN("yes") if r.stayed_open else RED("no"),
        ))

    all_connected = all(r.connected for r in receivers)
    all_got_frames = all(r.frame_count > 0 for r in receivers)
    all_open = all(r.stayed_open for r in receivers)

    all_ok &= line(all_connected, "all %d receivers completed the handshake" % n)
    all_ok &= line(all_got_frames, "every receiver received video frames")
    all_ok &= line(all_open, "every receiver stayed connected for %.1fs" % duration)

    for r in receivers:
        r.close()
    return all_ok


def test_late_joiner_does_not_kick(host, port,
                                   pre=2.0, post=2.0):
    """
    R1 connects and streams. R2 joins later. R1 must STILL be open and
    STILL be receiving frames afterwards -- this is the exact behaviour
    the single-client draining bug broke.
    """
    print(BOLD("\nTest 2: a late joiner must not kick an existing receiver"))
    r1 = Receiver("R1", host, port)
    if not r1.connect():
        print("  " + RED("ERROR") + " R1 could not connect: %s" % r1.error)
        r1.close()
        return False

    # R1 streams alone for a while.
    t1 = threading.Thread(target=r1.read, args=(pre,))
    t1.start()
    t1.join()
    frames_before = r1.frame_count
    print("  R1 streamed alone: frames=%d open=%s" % (
        frames_before, GREEN("yes") if r1.stayed_open else RED("no")))

    if not r1.stayed_open:
        r1.close()
        line(False, "R1 stayed open before the late joiner")
        return False

    # R2 joins.
    r2 = Receiver("R2", host, port)
    if not r2.connect():
        print("  " + RED("ERROR") + " R2 could not connect: %s" % r2.error)
        r1.close()
        r2.close()
        return False
    print("  R2 joined while R1 was streaming")

    # Both keep streaming.
    t1 = threading.Thread(target=r1.read, args=(post,))
    t2 = threading.Thread(target=r2.read, args=(post,))
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    frames_after_r1 = r1.frame_count - frames_before

    print("  after join: R1 frames(+%d) open=%s | R2 frames=%d open=%s" % (
        frames_after_r1,
        GREEN("yes") if r1.stayed_open else RED("no"),
        r2.frame_count,
        GREEN("yes") if r2.stayed_open else RED("no"),
    ))

    ok = True
    ok &= line(r1.stayed_open, "R1 was NOT kicked when R2 joined")
    ok &= line(frames_after_r1 > 0, "R1 kept receiving frames after R2 joined")
    ok &= line(r2.frame_count > 0, "R2 received frames")
    ok &= line(r2.stayed_open, "R2 stayed connected")

    r1.close()
    r2.close()
    return ok


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    if subprocess.run(["which", "gcc"], capture_output=True).returncode != 0:
        print(RED("ERROR") + " gcc is required to build the test server")
        return 2

    workdir = tempfile.mkdtemp(prefix="camtest_")
    binary = os.path.join(workdir, "test_http_server")

    if not build_server(binary):
        return 2

    port = find_free_port()
    host = "127.0.0.1"

    print(BOLD("Starting") + " server on %s:%d" % (host, port))
    proc = subprocess.Popen(
        [binary, "-a", "0.0.0.0", "-p", str(port)],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    overall = True
    try:
        if not wait_for_status(host, port):
            overall = False
        else:
            print(GREEN("server ready"))
            overall &= test_simultaneous(host, port, n=4, duration=4.0)
            overall &= test_late_joiner_does_not_kick(host, port)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print(BOLD("\n=== Result ==="))
    if overall:
        print(GREEN("ALL TESTS PASSED") + " -- multiple receivers stream together.")
        return 0
    print(RED("TESTS FAILED"))
    return 1


if __name__ == "__main__":
    sys.exit(main())
