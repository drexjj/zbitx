#!/usr/bin/env python3
"""
tci_panel.py - a lightweight zBitx control panel for the Pi Zero 2 W desktop.

Talks to zbitx's built-in TCI server (src/tci.c, TCP 50001 by default) over
a WebSocket, using only the Python standard library plus Tkinter. Based on
the WebSocket code in maxibitx's tools/tci_client.py (GPL-3.0).

    tci_panel.py [--host H] [--port P] [--fullscreen] [--waterfall]

Shows the dial frequency, mode, S-meter, TX state and a UTC clock, and has
controls for band, mode, tuning step, volume, mute, drive, RIT and PTT.

Tuning: mouse wheel over the frequency (or Up/Down, PgUp/PgDn keys) moves
by the selected step; click the frequency to type one in (kHz or Hz).

--waterfall adds a +-24 kHz spectrum/waterfall from TCI's I/Q stream (click
it to tune). It needs numpy (sudo apt install python3-numpy) and costs CPU
on both the GUI and the radio, so it is off by default.

PTT keys the radio the same way Hamlib PTT does (TCI trx without the "tci"
audio source), so the transmit audio is the loopback, not the mic.
"""

import argparse
import base64
import hashlib
import os
import queue
import select
import socket
import struct
import threading
import time
import tkinter as tk
from tkinter import simpledialog

try:
    import numpy as np
except ImportError:
    np = None

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

MODES = ["USB", "LSB", "CW", "CWR", "AM", "DIGU"]
# band name -> (default dial frequency, low edge, high edge), Hz
BANDS = [
    ("80", 3573000, 3500000, 4000000),
    ("60", 5357000, 5330000, 5410000),
    ("40", 7074000, 7000000, 7300000),
    ("30", 10136000, 10100000, 10150000),
    ("20", 14074000, 14000000, 14350000),
    ("17", 18100000, 18068000, 18168000),
    ("15", 21074000, 21000000, 21450000),
    ("12", 24915000, 24890000, 24990000),
    ("10", 28074000, 28000000, 29700000),
]
STEPS = [("10", 10), ("100", 100), ("500", 500), ("1k", 1000), ("10k", 10000)]

BG = "#1e2329"
PANEL = "#2a3038"
FG = "#e6e6e6"
DIM = "#8a939e"
ACCENT = "#36c5f0"
ON = "#2e7d32"
TX_RED = "#c62828"


# ---------------------------------------------------------------- WebSocket

class WebSocket:
    """Minimal RFC 6455 client: text and binary frames, ping/pong, close."""

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
            raise ConnectionError("handshake refused")
        self.sock.settimeout(None)
        self.buf = b""
        self.send_lock = threading.Lock()

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
            n = struct.unpack(">H", self._need(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._need(8))[0]
        return h[0] & 0x0F, self._need(n) if n else b""

    def _need(self, n):
        d = self._read(n, 5)
        if d is None:
            raise ConnectionError("timed out inside a frame")
        return d

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
        if np is not None and n > 64:
            m = np.frombuffer((mask * (n // 4 + 1))[:n], dtype=np.uint8)
            body = (np.frombuffer(payload, dtype=np.uint8) ^ m).tobytes()
        else:
            body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        with self.send_lock:
            self.sock.sendall(head + mask + body)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ------------------------------------------------------------- TCI link

class TciLink(threading.Thread):
    """Background thread: connects (and reconnects), queues text commands
    and I/Q frames for the GUI thread."""

    def __init__(self, host, port, want_iq):
        super().__init__(daemon=True)
        self.host, self.port, self.want_iq = host, port, want_iq
        self.inbox = queue.Queue()
        self.iq = queue.Queue(maxsize=64)
        self.ws = None
        self.running = True

    def send(self, cmd):
        ws = self.ws
        if ws is None:
            return
        try:
            ws.send(cmd)
        except OSError:
            pass

    def run(self):
        while self.running:
            try:
                self.inbox.put(("status", f"connecting to {self.host}:{self.port}"))
                self.ws = WebSocket(self.host, self.port)
                self.inbox.put(("status", "connected"))
                self._session()
            except (ConnectionError, OSError) as e:
                self.inbox.put(("status", f"offline: {e}"))
            finally:
                if self.ws:
                    self.ws.close()
                self.ws = None
                self.inbox.put(("disconnected", None))
            time.sleep(2)

    def _session(self):
        ws = self.ws
        ready = False
        while self.running:
            frame = ws.recv(1.0)
            if frame is None:
                continue
            op, data = frame
            if op == 1:
                for cmd in data.decode(errors="replace").split(";"):
                    cmd = cmd.strip()
                    if not cmd:
                        continue
                    self.inbox.put(("cmd", cmd))
                    if cmd.lower() == "ready" and not ready:
                        ready = True
                        ws.send("rx_sensors_enable:true,200;")
                        if self.want_iq:
                            ws.send("iq_samplerate:48000;")
                            ws.send("iq_start:0;")
            elif op == 2 and len(data) >= 64:
                f = struct.unpack("<16I", data[:64])
                if f[6] == 0 and f[2] == 3:  # I/Q stream, float32
                    try:
                        self.iq.put_nowait((f[1], data[64:64 + 4 * f[5]]))
                    except queue.Full:
                        pass  # GUI is behind; drop rather than lag
            elif op == 9:
                ws.send(data, opcode=10)
            elif op == 8:
                raise ConnectionError("server closed the connection")


# ------------------------------------------------------------------- GUI

def fmt_freq(hz):
    s = f"{hz:,}".replace(",", ".")
    return s


def s_units(dbm):
    """S-meter text from dBm (S9 = -73 dBm, 6 dB per S unit)."""
    if dbm >= -73:
        return f"S9+{int(round(dbm + 73))}"
    s = max(0, int(round(9 + (dbm + 73) / 6)))
    return f"S{s}"


class Panel:
    def __init__(self, root, link, waterfall):
        self.root, self.link = root, link
        self.freq = 0
        self.mode = ""
        self.tx = False
        self.mute = False
        self.rit_on = False
        self.rit = 0
        self.step = 1000
        self.connected = False
        self.suppress = {}          # control -> time until which echoes are ignored
        self.band_mem = {}

        root.title("zBitx")
        root.configure(bg=BG)
        root.minsize(460, 300)

        self._build_top()
        self._build_bands()
        self._build_modes()
        self.wf = Waterfall(root, self) if waterfall else None
        self._build_controls()
        self._build_status()

        root.bind("<Up>", lambda e: self.tune_by(self.step))
        root.bind("<Down>", lambda e: self.tune_by(-self.step))
        root.bind("<Prior>", lambda e: self.tune_by(10 * self.step))
        root.bind("<Next>", lambda e: self.tune_by(-10 * self.step))
        root.bind("<Escape>", lambda e: root.attributes("-fullscreen", False))

        self.root.after(50, self.poll)
        self.tick_clock()

    # -- layout --

    def _btn(self, parent, text, cmd, width=4):
        return tk.Button(parent, text=text, command=cmd, width=width, bg=PANEL, fg=FG,
                         activebackground=ACCENT, activeforeground=BG, relief="flat",
                         highlightthickness=0, bd=0, padx=2, pady=4, font=("DejaVu Sans", 10, "bold"))

    def _build_top(self):
        top = tk.Frame(self.root, bg=BG)
        top.pack(fill="x", padx=6, pady=(6, 2))

        left = tk.Frame(top, bg=BG)
        left.pack(side="left", fill="x", expand=True)
        self.lbl_freq = tk.Label(left, text="--.---.---", bg=BG, fg=ACCENT,
                                 font=("DejaVu Sans Mono", 30, "bold"), cursor="hand2")
        self.lbl_freq.pack(anchor="w")
        self.lbl_freq.bind("<Button-1>", self.enter_freq)
        self.lbl_freq.bind("<MouseWheel>", self.on_wheel)
        self.lbl_freq.bind("<Button-4>", lambda e: self.tune_by(self.step))
        self.lbl_freq.bind("<Button-5>", lambda e: self.tune_by(-self.step))

        meter = tk.Frame(left, bg=BG)
        meter.pack(anchor="w", fill="x")
        self.smeter = tk.Canvas(meter, width=200, height=14, bg=PANEL, highlightthickness=0)
        self.smeter.pack(side="left")
        self.lbl_s = tk.Label(meter, text="S0", bg=BG, fg=FG, width=7, anchor="w",
                              font=("DejaVu Sans Mono", 10))
        self.lbl_s.pack(side="left", padx=4)
        self.lbl_rit = tk.Label(meter, text="", bg=BG, fg="#ffb300", font=("DejaVu Sans Mono", 10))
        self.lbl_rit.pack(side="left")

        right = tk.Frame(top, bg=BG)
        right.pack(side="right", anchor="n")
        self.lbl_clock = tk.Label(right, text="--:--:--", bg=BG, fg=FG,
                                  font=("DejaVu Sans Mono", 14, "bold"))
        self.lbl_clock.pack(anchor="e")
        self.lbl_mode = tk.Label(right, text="---", bg=BG, fg=FG, font=("DejaVu Sans", 12, "bold"))
        self.lbl_mode.pack(anchor="e")
        self.lbl_tx = tk.Label(right, text=" RX ", bg=ON, fg="white", font=("DejaVu Sans", 11, "bold"))
        self.lbl_tx.pack(anchor="e", pady=(2, 0))

    def _build_bands(self):
        row = tk.Frame(self.root, bg=BG)
        row.pack(fill="x", padx=6, pady=2)
        self.band_btns = {}
        for name, f, lo, hi in BANDS:
            b = self._btn(row, name + "m", lambda n=name: self.set_band(n), width=3)
            b.pack(side="left", padx=1, expand=True, fill="x")
            self.band_btns[name] = b

    def _build_modes(self):
        row = tk.Frame(self.root, bg=BG)
        row.pack(fill="x", padx=6, pady=2)
        self.mode_btns = {}
        for m in MODES:
            b = self._btn(row, m, lambda m=m: self.link.send(f"modulation:0,{m.lower()};"))
            b.pack(side="left", padx=1, expand=True, fill="x")
            self.mode_btns[m] = b
        tk.Label(row, text=" step", bg=BG, fg=DIM).pack(side="left")
        self.step_var = tk.StringVar(value="1k")
        om = tk.OptionMenu(row, self.step_var, *[s for s, _ in STEPS], command=self.set_step)
        om.configure(bg=PANEL, fg=FG, activebackground=ACCENT, highlightthickness=0,
                     relief="flat", width=4)
        om["menu"].configure(bg=PANEL, fg=FG)
        om.pack(side="left", padx=2)

    def _scale(self, parent, label, lo, hi, cmd):
        f = tk.Frame(parent, bg=BG)
        tk.Label(f, text=label, bg=BG, fg=DIM, width=4, anchor="w").pack(side="left")
        s = tk.Scale(f, from_=lo, to=hi, orient="horizontal", showvalue=True, bg=BG, fg=FG,
                     troughcolor=PANEL, highlightthickness=0, bd=0, sliderlength=18,
                     length=150, command=cmd)
        s.pack(side="left", fill="x", expand=True)
        return f, s

    def _build_controls(self):
        row = tk.Frame(self.root, bg=BG)
        row.pack(fill="x", padx=6, pady=2)
        fv, self.sc_vol = self._scale(row, "VOL", 0, 100, self.on_volume)
        fv.pack(side="left", fill="x", expand=True)
        fd, self.sc_drive = self._scale(row, "PWR", 0, 100, self.on_drive)
        fd.pack(side="left", fill="x", expand=True)

        row2 = tk.Frame(self.root, bg=BG)
        row2.pack(fill="x", padx=6, pady=(2, 4))
        self.btn_mute = self._btn(row2, "MUTE", self.toggle_mute, width=6)
        self.btn_mute.pack(side="left", padx=1)
        self.btn_rit = self._btn(row2, "RIT", self.toggle_rit, width=5)
        self.btn_rit.pack(side="left", padx=1)
        self._btn(row2, "-", lambda: self.rit_by(-10), width=2).pack(side="left", padx=1)
        self._btn(row2, "0", lambda: self.rit_set(0), width=2).pack(side="left", padx=1)
        self._btn(row2, "+", lambda: self.rit_by(10), width=2).pack(side="left", padx=1)
        self.btn_ptt = tk.Button(row2, text="PTT", command=self.toggle_ptt, width=10,
                                 bg="#5d1f1f", fg="white", activebackground=TX_RED,
                                 relief="flat", bd=0, highlightthickness=0, pady=4,
                                 font=("DejaVu Sans", 11, "bold"))
        self.btn_ptt.pack(side="right", padx=1)

    def _build_status(self):
        self.lbl_status = tk.Label(self.root, text="", bg=BG, fg=DIM, anchor="w",
                                   font=("DejaVu Sans", 8))
        self.lbl_status.pack(fill="x", padx=6, pady=(0, 3), side="bottom")

    # -- actions --

    def _hold(self, key, secs=0.6):
        self.suppress[key] = time.monotonic() + secs

    def _held(self, key):
        return time.monotonic() < self.suppress.get(key, 0)

    def tune(self, hz):
        hz = max(500000, min(32000000, int(hz)))
        self.freq = hz
        self.show_freq()
        self._hold("vfo")
        self.link.send(f"vfo:0,0,{hz};")

    def tune_by(self, delta):
        if not self.freq:
            return
        f = self.freq + delta
        self.tune(f - f % abs(delta))  # land on the step grid

    def on_wheel(self, e):
        self.tune_by(self.step if e.delta > 0 else -self.step)

    def enter_freq(self, _e=None):
        s = simpledialog.askstring("Frequency", "kHz (e.g. 14074) or Hz:", parent=self.root)
        if not s:
            return
        try:
            v = float(s.replace(",", ""))
        except ValueError:
            return
        self.tune(v * 1000 if v < 100000 else v)

    def set_step(self, label):
        self.step = dict(STEPS)[label]

    def band_of(self, hz):
        for name, f, lo, hi in BANDS:
            if lo <= hz <= hi:
                return name
        return None

    def set_band(self, name):
        cur = self.band_of(self.freq)
        if cur:
            self.band_mem[cur] = self.freq
        default = next(f for n, f, lo, hi in BANDS if n == name)
        self.tune(self.band_mem.get(name, default))

    def on_volume(self, v):
        if self._held("volume_in"):
            return
        db = -60 if int(v) <= 0 else round(-60 + 0.6 * int(v))
        self._hold("volume")
        self.link.send(f"volume:{db};")

    def on_drive(self, v):
        if self._held("drive_in"):
            return
        self._hold("drive")
        self.link.send(f"drive:0,{int(v)};")

    def toggle_mute(self):
        self.link.send(f"mute:{'false' if self.mute else 'true'};")

    def toggle_rit(self):
        self.link.send(f"rit_enable:0,{'false' if self.rit_on else 'true'};")

    def rit_set(self, hz):
        self.link.send(f"rit_offset:0,{max(-9999, min(9999, hz))};")

    def rit_by(self, d):
        self.rit_set(self.rit + d)

    def toggle_ptt(self):
        self.link.send(f"trx:0,{'false' if self.tx else 'true'};")

    # -- incoming --

    def handle(self, cmd):
        name, _, args = cmd.partition(":")
        name = name.lower()
        a = args.split(",") if args else []
        try:
            if name == "vfo" and len(a) >= 3 and a[1] == "0":
                if not self._held("vfo"):
                    self.freq = int(a[2])
                    self.show_freq()
            elif name == "modulation" and len(a) >= 2:
                self.mode = a[1].upper()
                self.lbl_mode.config(text=self.mode)
                for m, b in self.mode_btns.items():
                    b.config(bg=ACCENT if m == self.mode else PANEL,
                             fg=BG if m == self.mode else FG)
            elif name == "trx" and len(a) >= 2:
                self.tx = a[1].lower() == "true"
                self.lbl_tx.config(text=" TX " if self.tx else " RX ", bg=TX_RED if self.tx else ON)
                self.btn_ptt.config(bg=TX_RED if self.tx else "#5d1f1f",
                                    text="TX - tap to stop" if self.tx else "PTT")
            elif name == "drive" and len(a) >= 2 and not self._held("drive"):
                self._hold("drive_in", 0.2)
                self.sc_drive.set(int(a[1]))
            elif name == "volume" and a and not self._held("volume"):
                db = int(float(a[0]))
                self._hold("volume_in", 0.2)
                self.sc_vol.set(0 if db <= -60 else round((db + 60) / 0.6))
            elif name == "mute" and a:
                self.mute = a[0].lower() == "true"
                self.btn_mute.config(bg=TX_RED if self.mute else PANEL)
            elif name == "rit_enable" and len(a) >= 2:
                self.rit_on = a[1].lower() == "true"
                self.btn_rit.config(bg="#b07800" if self.rit_on else PANEL)
                self.show_rit()
            elif name == "rit_offset" and len(a) >= 2:
                self.rit = int(a[1])
                self.show_rit()
            elif name in ("rx_sensors", "rx_smeter") and a:
                self.show_smeter(float(a[-1]))
        except ValueError:
            pass

    def show_freq(self):
        self.lbl_freq.config(text=fmt_freq(self.freq))
        band = self.band_of(self.freq)
        for n, b in self.band_btns.items():
            b.config(bg=ACCENT if n == band else PANEL, fg=BG if n == band else FG)
        if self.wf:
            self.wf.redraw_scale()

    def show_rit(self):
        self.lbl_rit.config(text=f"RIT {self.rit:+d}" if self.rit_on else "")

    def show_smeter(self, dbm):
        c = self.smeter
        c.delete("all")
        # -127 dBm (S0) .. -43 dBm (S9+30) across the bar
        frac = max(0.0, min(1.0, (dbm + 127) / 84))
        w = int(c.winfo_width() or 200)
        s9 = int(w * 54 / 84)
        fill = int(w * frac)
        c.create_rectangle(0, 0, min(fill, s9), 14, fill="#43a047", width=0)
        if fill > s9:
            c.create_rectangle(s9, 0, fill, 14, fill="#e53935", width=0)
        c.create_line(s9, 0, s9, 14, fill=DIM)
        self.lbl_s.config(text=s_units(dbm))

    def poll(self):
        try:
            for _ in range(200):
                kind, val = self.link.inbox.get_nowait()
                if kind == "cmd":
                    self.handle(val)
                elif kind == "status":
                    self.connected = val == "connected"
                    self.lbl_status.config(text=val)
                elif kind == "disconnected":
                    self.connected = False
                    self.lbl_freq.config(fg=DIM)
                if self.connected:
                    self.lbl_freq.config(fg=ACCENT)
        except queue.Empty:
            pass
        if self.wf:
            self.wf.pump()
        self.root.after(50, self.poll)

    def tick_clock(self):
        self.lbl_clock.config(text=time.strftime("%H:%M:%SZ", time.gmtime()))
        self.root.after(1000 - int(time.time() * 1000) % 1000 + 5, self.tick_clock)


class Waterfall:
    """Spectrum + waterfall of TCI's I/Q stream (dial at the centre,
    positive frequencies above the dial)."""

    N = 1024
    H = 110
    FPS = 8

    def __init__(self, root, panel):
        self.panel = panel
        self.c = tk.Canvas(root, height=self.H + 16, width=300, bg="black", highlightthickness=0, cursor="crosshair")
        self.c.pack(fill="both", expand=True, padx=6, pady=2)
        self.c.bind("<Button-1>", self.click)
        self.c.bind("<Configure>", lambda e: self.resize())
        self.w = 0
        self.h = self.H
        self.img = None
        self.rows = None
        self.buf = np.zeros(0, dtype=np.complex64)
        self.rate = 48000
        self.avg = None
        self.last = 0.0
        self.win = np.hanning(self.N).astype(np.float32)
        self.lo, self.hi = -120.0, -50.0
        x = np.linspace(0, 1, 256)
        self.cmap = (np.stack([np.clip(1.5 * x - 0.3, 0, 1) * 255,
                               np.clip(2.0 * x - 0.6, 0, 1) * 255 * 0.9 + np.clip(x * 3, 0, 1) * 25,
                               np.clip(np.sin(x * np.pi) * 1.2, 0, 1) * 255 * (x < 0.7) + (x >= 0.7) * 120],
                              axis=1)).astype(np.uint8)

    def resize(self):
        # width capped at 800 px to bound the per-frame work on a Pi Zero
        w = max(100, min(800, self.c.winfo_width()))
        h = max(40, self.c.winfo_height() - 16)
        if (w, h) != (self.w, self.h) or self.rows is None:
            self.w, self.h = w, h
            self.rows = np.zeros((h, w, 3), dtype=np.uint8)
            self.redraw_scale()

    def redraw_scale(self):
        c = self.c
        c.delete("scale")
        if not self.w:
            return
        half = self.rate / 2
        top_khz = int(half // 5000) * 5
        for khz in range(-top_khz, top_khz + 1, 5):
            x = (khz * 1000 + half) / self.rate * self.w
            c.create_line(x, self.h, x, self.h + 4, fill=DIM, tags="scale")
            if khz % 10 == 0:
                c.create_text(x, self.h + 10, text=f"{khz:+d}" if khz else "0", fill=DIM,
                              font=("DejaVu Sans", 7), tags="scale")
        mid = self.w / 2
        c.create_line(mid, 0, mid, self.h, fill="#ff5252", dash=(2, 3), tags="scale")

    def click(self, e):
        if not self.w or not self.panel.freq:
            return
        off = (e.x / self.w - 0.5) * self.rate
        self.panel.tune(round((self.panel.freq + off) / 10) * 10)

    def pump(self):
        q = self.panel.link.iq
        chunks = []
        while True:
            try:
                rate, data = q.get_nowait()
            except queue.Empty:
                break
            self.rate = rate
            chunks.append(np.frombuffer(data, dtype="<f4"))
        if chunks:
            f = np.concatenate(chunks)
            iq = (f[0::2] + 1j * f[1::2]).astype(np.complex64)
            self.buf = np.concatenate([self.buf, iq])[-4 * self.N:]
        now = time.monotonic()
        if len(self.buf) < self.N or now - self.last < 1.0 / self.FPS or not self.w:
            return
        self.last = now
        spec = np.fft.fftshift(np.fft.fft(self.buf[-self.N:] * self.win))
        p = 20 * np.log10(np.abs(spec) / self.N + 1e-12)
        self.avg = p if self.avg is None or len(self.avg) != len(p) else 0.6 * self.avg + 0.4 * p
        # slow auto-range: noise floor at the bottom fifth
        floor = float(np.percentile(self.avg, 20))
        self.lo += 0.1 * ((floor - 5) - self.lo)
        self.hi = self.lo + 70
        cols = np.interp(np.linspace(0, self.N - 1, self.w), np.arange(self.N), self.avg)
        idx = np.clip((cols - self.lo) / (self.hi - self.lo) * 255, 0, 255).astype(np.uint8)
        self.rows[1:] = self.rows[:-1]
        self.rows[0] = self.cmap[idx]
        frame = self.rows.copy()
        # spectrum trace over the top 40 px
        top = 40
        ys = (top - np.clip((cols - self.lo) / (self.hi - self.lo), 0, 1) * top).astype(int)
        under = np.arange(top)[:, None] >= ys[None, :]   # (top, w) mask below the trace
        band = frame[:top]
        band[under] = band[under] // 2 + np.array([0, 90, 110], dtype=np.uint8)
        ppm = b"P6 %d %d 255\n" % (self.w, self.h) + frame.tobytes()
        self.img = tk.PhotoImage(data=ppm, format="PPM")
        self.c.delete("wf")
        self.c.create_image(0, 0, image=self.img, anchor="nw", tags="wf")
        self.c.tag_raise("scale")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=50001)
    ap.add_argument("--fullscreen", action="store_true")
    ap.add_argument("--waterfall", action="store_true", help="show the I/Q waterfall (needs numpy)")
    ap.add_argument("--geometry", default="", help="Tk window geometry, e.g. 480x320+0+0")
    a = ap.parse_args()

    if a.waterfall and np is None:
        print("tci_panel: --waterfall needs numpy (sudo apt install python3-numpy); continuing without it")
        a.waterfall = False

    root = tk.Tk()
    if a.geometry:
        root.geometry(a.geometry)
    if a.fullscreen:
        root.attributes("-fullscreen", True)
    link = TciLink(a.host, a.port, a.waterfall)
    Panel(root, link, a.waterfall)
    link.start()
    try:
        root.mainloop()
    finally:
        link.running = False
        if link.ws:
            link.send("iq_stop:0;")
            link.ws.close()


if __name__ == "__main__":
    main()
