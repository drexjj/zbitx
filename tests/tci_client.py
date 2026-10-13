#!/usr/bin/env python3
"""
tci_client.py - a small TCI client for trying zbitx's TCI server (src/tci.c)
from a laptop. Standard library only. From maxibitx's tools/tci_client.py.

    tci_client.py [--host H] [--port P] [options]

With no options it connects, prints the initialization burst, checks it
against the rules JTDX and Hamlib depend on (lowercase keywords, one
command per message, start; before ready;, within 1.5 s), and exits.

    -c "vfo:0,0,7074000;"   send commands after ready; (repeatable), and
                            print what comes back for --listen seconds
    --listen S              how long to print replies (default 1)
    --audio S               start receive audio for S seconds and report its
                            format, frame rate and level; --wav FILE saves it
    --iq S                  start I/Q (--iq-rate 48000|96000) for S seconds and
                            report its format and the strongest frequency
    --tx-tone HZ            TRANSMITS: keys with trx:0,true,tci and answers
                            TX_CHRONO with a tone of HZ at --tx-level (0-1,
                            default 0.3) for --tx-seconds (default 3). Use a
                            dummy load, in USB or DIGU.

Examples:
    tci_client.py --host zbitx.local
    tci_client.py --host zbitx.local -c "modulation:0,digu;" -c "vfo:0,0,14074000;"
    tci_client.py --host zbitx.local --audio 5 --wav rx.wav
    tci_client.py --host zbitx.local --iq 3 --iq-rate 48000

src/docs/tci.md.
"""

import argparse
import base64
import cmath
import hashlib
import math
import os
import select
import socket
import struct
import sys
import time
import wave

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
STREAM_NAMES = {0: "IQ", 1: "RX_AUDIO", 2: "TX_AUDIO", 3: "TX_CHRONO", 4: "LINE_OUT"}
TYPE_NAMES = {0: "int16", 1: "int24", 2: "int32", 3: "float32"}


class WebSocket:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                raise ConnectionError("connection closed during the handshake")
            head += chunk
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        if b" 101 " not in head.split(b"\r\n")[0] or want.encode() not in head:
            raise ConnectionError("handshake refused: " + head.decode(errors="replace").strip())
        self.buf = b""

    def _read(self, n, timeout):
        end = time.monotonic() + timeout
        while len(self.buf) < n:
            left = end - time.monotonic()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                return None
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self, timeout):
        """Returns (opcode, payload), or None on timeout."""
        h = self._read(2, timeout)
        if h is None:
            return None
        n = h[1] & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._read(2, 5))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._read(8, 5))[0]
        return h[0] & 0x0F, self._read(n, 5) if n else b""

    def send(self, payload, opcode=1):
        if isinstance(payload, str):
            payload = payload.encode()
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack(">Q", n)
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(head + mask + body)


def header(data):
    f = struct.unpack("<16I", data[:64])
    return {"receiver": f[0], "rate": f[1], "format": f[2], "length": f[5], "type": f[6],
            "channels": f[7]}


def samples(data, h):
    fmt, n = h["format"], h["length"]
    p = data[64:]
    if fmt == 3:
        return list(struct.unpack(f"<{n}f", p[:4 * n]))
    if fmt == 0:
        return [v / 32768 for v in struct.unpack(f"<{n}h", p[:2 * n])]
    if fmt == 2:
        return [v / 2147483648 for v in struct.unpack(f"<{n}i", p[:4 * n])]
    return [int.from_bytes(p[3 * k:3 * k + 3], "little", signed=True) / 8388608 for k in range(n)]


def fft(x):
    n = len(x)
    if n == 1:
        return x
    even, odd = fft(x[0::2]), fft(x[1::2])
    tw = [cmath.exp(-2j * math.pi * k / n) * odd[k] for k in range(n // 2)]
    return [even[k] + tw[k] for k in range(n // 2)] + [even[k] - tw[k] for k in range(n // 2)]


def check_init(msgs, ms):
    problems = []
    for m in msgs:
        key = m.split(":")[0].split(";")[0]
        if key != key.lower() or not m.endswith(";") or m.count(";") != 1:
            problems.append(f"not one lowercase command: {m!r}")
    if "start;" not in msgs or "ready;" not in msgs or msgs.index("start;") > msgs.index("ready;"):
        problems.append("start; must come before ready;")
    if ms > 1500:
        problems.append(f"ready; took {ms} ms (JTDX allows 1500)")
    if len(msgs) >= 256:
        problems.append(f"{len(msgs)} messages before ready; (Hamlib reads 256)")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=50001)
    ap.add_argument("-c", "--command", action="append", default=[])
    ap.add_argument("--listen", type=float, default=1.0)
    ap.add_argument("--audio", type=float, default=0)
    ap.add_argument("--wav")
    ap.add_argument("--iq", type=float, default=0)
    ap.add_argument("--iq-rate", type=int, default=48000)
    ap.add_argument("--tx-tone", type=float, default=0)
    ap.add_argument("--tx-level", type=float, default=0.3)
    ap.add_argument("--tx-seconds", type=float, default=3.0)
    ap.add_argument("-q", "--quiet", action="store_true", help="don't print the initialization burst")
    a = ap.parse_args()

    t0 = time.monotonic()
    ws = WebSocket(a.host, a.port)
    msgs = []
    while True:
        frame = ws.recv(2.0)
        if frame is None:
            break
        if frame[0] == 1:
            msgs.append(frame[1].decode())
            if msgs[-1] == "ready;":
                break
    ms = int((time.monotonic() - t0) * 1000)
    if not a.quiet:
        print("\n".join(msgs))
    problems = check_init(msgs, ms)
    print(f"-- {len(msgs)} messages, ready; after {ms} ms: " + ("OK" if not problems else "PROBLEMS"))
    for p in problems:
        print("   " + p)

    def pump(seconds, on_binary=None):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            frame = ws.recv(end - time.monotonic())
            if frame is None:
                break
            if frame[0] == 1:
                print("<- " + frame[1].decode())
            elif frame[0] == 2 and on_binary and len(frame[1]) >= 64:
                on_binary(frame[1], header(frame[1]))

    if a.command:
        for c in a.command:
            print("-> " + c)
            ws.send(c)
        pump(a.listen)

    if a.audio:
        got = []
        frames = []

        def on_audio(data, h):
            if h["type"] == 1:
                frames.append(h)
                got.extend(samples(data, h)[::h["channels"] or 1])

        ws.send("audio_start:0;")
        pump(a.audio, on_audio)
        ws.send("audio_stop:0;")
        if frames:
            h = frames[0]
            rms = math.sqrt(sum(v * v for v in got) / len(got)) if got else 0
            print(f"-- RX audio: {len(frames)} frames, {h['rate']} Hz {TYPE_NAMES.get(h['format'])}, "
                  f"{h['channels']} ch, length {h['length']}, {len(frames) / a.audio:.1f} frames/s, "
                  f"{20 * math.log10(rms + 1e-12):.1f} dBFS rms")
            if a.wav:
                with wave.open(a.wav, "wb") as w:
                    w.setnchannels(1)
                    w.setsampwidth(2)
                    w.setframerate(h["rate"])
                    w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(v * 32767))))
                                           for v in got))
                print(f"-- saved {len(got)} samples to {a.wav}")
        else:
            print("-- no RX audio frames arrived")

    if a.iq:
        iq = []
        frames = []

        def on_iq(data, h):
            if h["type"] == 0:
                frames.append(h)
                s = samples(data, h)
                iq.extend(complex(s[k], s[k + 1]) for k in range(0, len(s) - 1, 2))

        ws.send(f"iq_samplerate:{a.iq_rate};")
        ws.send("iq_start:0;")
        pump(a.iq, on_iq)
        ws.send("iq_stop:0;")
        if len(iq) >= 4096:
            h = frames[0]
            spec = fft(iq[-4096:])
            k = max(range(4096), key=lambda i: abs(spec[i]))
            hz = (k if k < 2048 else k - 4096) * h["rate"] / 4096
            print(f"-- I/Q: {len(frames)} frames at {h['rate']} Hz, length {h['length']}, "
                  f"strongest at {hz:+.0f} Hz from the dial")
        else:
            print("-- not enough I/Q arrived")

    if a.tx_tone:
        phase = [0]
        answered = [0]

        def on_chrono(data, h):
            if h["type"] != 3:
                return
            n = h["length"]
            vals = []
            for _ in range(n // 2):
                v = a.tx_level * math.sin(2 * math.pi * a.tx_tone * phase[0] / 48000)
                phase[0] += 1
                vals += [v, v]
            out = bytearray(data[:64])
            out[24:28] = struct.pack("<I", 2)
            ws.send(bytes(out) + struct.pack(f"<{n}f", *vals), opcode=2)
            answered[0] += 1

        print(f"-- TRANSMITTING a {a.tx_tone:.0f} Hz tone for {a.tx_seconds} s")
        ws.send("trx:0,true,tci;")
        try:
            pump(a.tx_seconds, on_chrono)
        finally:
            ws.send("trx:0,false;")
        pump(0.5)
        print(f"-- answered {answered[0]} TX_CHRONO requests")


if __name__ == "__main__":
    try:
        main()
    except (ConnectionError, OSError) as e:
        sys.exit(f"tci_client: {e}")
