#!/usr/bin/env python3
"""
tci_panel.py - a lightweight zBitx control panel for the Pi Zero 2 W desktop.

Talks to zbitx's built-in TCI server (src/tci.c, TCP 50001 by default) over
a WebSocket, using only the Python standard library plus Tkinter. Based on
the WebSocket code in maxibitx's tools/tci_client.py (GPL-3.0).

    tci_panel.py [--host H] [--port P] [--fullscreen] [--no-waterfall] [--fps N]

Shows the dial frequency, mode, S-meter, TX state and a UTC clock, and has
controls for band, mode, tuning step, volume, mute, drive, RIT and PTT.

Tuning: mouse wheel over the frequency (or Up/Down, PgUp/PgDn keys) moves
by the selected step; click the frequency to type one in (kHz or Hz).

The waterfall (click it to tune) draws the spectrum the radio already
computes for its web UI and front panel, asked for with zbitx's
zbitx_spectrum TCI extension, so it adds almost no work on the radio side.
Its span follows the radio's SPAN control. --fps sets its rate (default 8);
--no-waterfall leaves it out.

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
    for the GUI thread, and keeps the latest spectrum frame in .spectrum."""

    SPECTRUM_TYPE = 100  # TCI_ZBITX_SPECTRUM in src/tci_stream.h

    def __init__(self, host, port, spectrum_ms):
        super().__init__(daemon=True)
        self.host, self.port, self.spectrum_ms = host, port, spectrum_ms
        self.inbox = queue.Queue()
        self.spectrum = None  # (span_hz, bytes), replaced by each new frame
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
                        if self.spectrum_ms:
                            ws.send(f"zbitx_spectrum:true,{self.spectrum_ms};")
            elif op == 2 and len(data) >= 64:
                f = struct.unpack("<16I", data[:64])
                if f[6] == self.SPECTRUM_TYPE:
                    self.spectrum = (f[1], data[64:64 + f[5]])
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
    def __init__(self, root, link, fps):
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
        # status and controls are packed from the bottom first, so on a small
        # screen the waterfall shrinks rather than pushing them off
        self._build_status()
        self._build_controls()
        self.wf = Waterfall(root, self, fps) if fps else None
        # GUI poll period: fast enough for the waterfall, otherwise 10 Hz
        self.poll_ms = min(100, int(1000 / fps)) if fps else 100

        root.bind("<Up>", lambda e: self.tune_by(self.step))
        root.bind("<Down>", lambda e: self.tune_by(-self.step))
        root.bind("<Prior>", lambda e: self.tune_by(10 * self.step))
        root.bind("<Next>", lambda e: self.tune_by(-10 * self.step))
        root.bind("<Escape>", lambda e: root.attributes("-fullscreen", False))

        self.root.after(self.poll_ms, self.poll)
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
        # bar items are made once and moved, not redrawn, on each reading
        s9 = 200 * 54 // 84
        self.sm_green = self.smeter.create_rectangle(0, 0, 0, 14, fill="#43a047", width=0)
        self.sm_red = self.smeter.create_rectangle(s9, 0, s9, 14, fill="#e53935", width=0)
        self.smeter.create_line(s9, 0, s9, 14, fill=DIM)
        self.sm_fill = -1
        self.sm_text = ""
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
        row2 = tk.Frame(self.root, bg=BG)
        row2.pack(side="bottom", fill="x", padx=6, pady=(2, 4))
        row = tk.Frame(self.root, bg=BG)
        row.pack(side="bottom", fill="x", padx=6, pady=2)
        fv, self.sc_vol = self._scale(row, "VOL", 0, 100, self.on_volume)
        fv.pack(side="left", fill="x", expand=True)
        fd, self.sc_drive = self._scale(row, "PWR", 0, 100, self.on_drive)
        fd.pack(side="left", fill="x", expand=True)

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
        # -127 dBm (S0) .. -43 dBm (S9+30) across the 200 px bar
        fill = int(200 * max(0.0, min(1.0, (dbm + 127) / 84)))
        s9 = 200 * 54 // 84
        if fill != self.sm_fill:
            self.sm_fill = fill
            self.smeter.coords(self.sm_green, 0, 0, min(fill, s9), 14)
            self.smeter.coords(self.sm_red, s9, 0, max(fill, s9), 14)
        text = s_units(dbm)
        if text != self.sm_text:
            self.sm_text = text
            self.lbl_s.config(text=text)

    def poll(self):
        try:
            for _ in range(200):
                kind, val = self.link.inbox.get_nowait()
                if kind == "cmd":
                    self.handle(val)
                elif kind == "status":
                    self.connected = val == "connected"
                    self.lbl_status.config(text=val)
                    self.lbl_freq.config(fg=ACCENT if self.connected else DIM)
                elif kind == "disconnected":
                    self.connected = False
                    self.lbl_freq.config(fg=DIM)
        except queue.Empty:
            pass
        if self.wf:
            frame, self.link.spectrum = self.link.spectrum, None
            if frame:
                self.wf.feed(*frame)
            self.wf.pump()
        self.root.after(self.poll_ms, self.poll)

    def tick_clock(self):
        self.lbl_clock.config(text=time.strftime("%H:%M:%SZ", time.gmtime()))
        self.root.after(1000 - int(time.time() * 1000) % 1000 + 5, self.tick_clock)


class Waterfall:
    """Waterfall + spectrum trace of the radio's own display spectrum (the
    zbitx_spectrum TCI extension): the dial at the centre, span set by the
    radio's SPAN control.

    Kept cheap for the Pi Zero: each frame writes one row into a PhotoImage
    used as a ring buffer and shown twice on the canvas, so scrolling is two
    coordinate moves instead of redrawing the image."""

    H = 110      # waterfall rows
    TRACE = 40   # spectrum trace height (overlaid on the newest rows)

    def __init__(self, root, panel, fps):
        self.panel = panel
        self.period = 1.0 / fps
        self.c = tk.Canvas(root, height=60, width=300, bg="black",
                           highlightthickness=0, cursor="crosshair")
        self.c.pack(fill="both", expand=True, padx=6, pady=2)
        self.c.bind("<Button-1>", self.click)
        self.c.bind("<Configure>", lambda e: self.resize())
        self.w = self.h = 0
        self.r = 0
        self.photo = None
        self.span = 0
        self.bins = None
        self.last = 0.0
        self.colmap = None   # canvas column -> bin index
        self.nbins = 0
        self.got_any = False
        self.c.create_line(0, 0, 0, 0, fill="#7fdcff", tags="trace")
        # 96 levels (0..95), dark blue -> cyan -> yellow -> red
        self.palette = [self._color(v / 95) for v in range(96)]

    @staticmethod
    def _color(x):
        stops = [(0.0, (0, 0, 20)), (0.25, (0, 30, 140)), (0.5, (0, 170, 200)),
                 (0.75, (240, 220, 0)), (1.0, (255, 40, 0))]
        for (x0, c0), (x1, c1) in zip(stops, stops[1:]):
            if x <= x1:
                t = (x - x0) / (x1 - x0)
                return "#%02x%02x%02x" % tuple(int(a + (b - a) * t) for a, b in zip(c0, c1))
        return "#ff2800"

    def resize(self):
        w = max(100, self.c.winfo_width())
        h = max(40, self.c.winfo_height() - 16)
        if (w, h) == (self.w, self.h):
            return
        self.w, self.h, self.r = w, h, 0
        self.photo = tk.PhotoImage(width=w, height=h)
        self.photo.put(self.palette[0], to=(0, 0, w, h))
        self.c.delete("wf")
        self.c.create_image(0, 0, image=self.photo, anchor="nw", tags=("wf", "wf_a"))
        self.c.create_image(0, h, image=self.photo, anchor="nw", tags=("wf", "wf_b"))
        # the second copy hangs below the waterfall; hide it under the scale strip
        self.c.create_rectangle(0, h, w, h + 40, fill="black", width=0, tags=("wf", "mask"))
        self.c.tag_lower("wf")
        self.c.tag_raise("mask", "wf_b")
        self.colmap = None
        self.redraw_scale()

    def redraw_scale(self):
        c = self.c
        c.delete("scale")
        if not self.w or not self.span:
            if self.w and not self.got_any:
                c.create_text(self.w / 2, self.h / 2, fill=DIM, tags="scale",
                              text="waiting for spectrum from the radio...")
            return
        half = self.span / 2
        step = 1 if self.span <= 3000 else 2 if self.span <= 8000 else 5
        top = int(half // (step * 1000)) * step
        for khz in range(-top, top + 1, step):
            x = (khz * 1000 + half) / self.span * self.w
            c.create_line(x, self.h, x, self.h + 4, fill=DIM, tags="scale")
            if khz % (2 * step) == 0:
                c.create_text(x, self.h + 10, text=f"{khz:+d}" if khz else "0", fill=DIM,
                              font=("DejaVu Sans", 7), tags="scale")
        mid = self.w / 2
        c.create_line(mid, 0, mid, self.h, fill="#ff5252", dash=(2, 3), tags="scale")

    def click(self, e):
        if not self.w or not self.span or not self.panel.freq:
            return
        off = (e.x / self.w - 0.5) * self.span
        self.panel.tune(round((self.panel.freq + off) / 10) * 10)

    def feed(self, span, data):
        """Latest frame from the link thread; drawn at most at fps."""
        if span != self.span:
            self.span = span
            self.got_any = True
            self.redraw_scale()
        self.bins = data

    def pump(self):
        now = time.monotonic()
        if self.bins is None or not self.w or now - self.last < self.period:
            return
        self.last = now
        bins, self.bins = self.bins, None
        n = len(bins)
        if not n:
            return
        if self.colmap is None or self.nbins != n:
            self.nbins = n
            self.colmap = [min(n - 1, int(x * n / self.w)) for x in range(self.w)]
        vals = [bins[i] for i in self.colmap]
        pal = self.palette
        # newest row goes in above the previous one; the ring wraps
        self.r = (self.r - 1) % self.h
        self.photo.put("{" + " ".join([pal[v if v < 96 else 95] for v in vals]) + "}",
                       to=(0, self.r))
        self.c.coords("wf_a", 0, -self.r)
        self.c.coords("wf_b", 0, self.h - self.r)
        k = self.TRACE / 95
        pts = []
        for x, v in enumerate(vals):
            pts += (x, self.TRACE - v * k)
        self.c.coords("trace", *pts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=50001)
    ap.add_argument("--fullscreen", action="store_true")
    ap.add_argument("--no-waterfall", action="store_true", help="leave the waterfall out")
    ap.add_argument("--fps", type=int, default=8, help="waterfall rows per second (default 8)")
    ap.add_argument("--geometry", default="", help="Tk window geometry, e.g. 480x320+0+0")
    a = ap.parse_args()

    fps = 0 if a.no_waterfall else max(1, min(20, a.fps))

    root = tk.Tk()
    if a.geometry:
        root.geometry(a.geometry)
    if a.fullscreen:
        root.attributes("-fullscreen", True)
    link = TciLink(a.host, a.port, int(1000 / fps) if fps else 0)
    Panel(root, link, fps)
    link.start()
    try:
        root.mainloop()
    finally:
        link.running = False
        if link.ws:
            link.ws.close()


if __name__ == "__main__":
    main()
