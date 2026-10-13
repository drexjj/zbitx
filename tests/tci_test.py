#!/usr/bin/env python3
"""Scripted checks of the TCI server against tests/tci_bench (no radio).

    make test-tci

Starts the bench, connects two clients the way JTDX, WSJT-X Improved and
Hamlib do, and checks: the initialization burst; that every set is echoed
to every client with the value in force and reads go only to the asker;
frequency, mode, drive, volume/mute, RIT and split; receive audio in two
formats; I/Q at 48 and 96 kHz on the right side of center; transmit audio
pulled with TX_CHRONO and arriving at the mic input as the tone sent; the
PTT rules; and CW text. Exit status is the number of failures.
"""
import math
import os
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tci_client import WebSocket, check_init, header, samples, fft  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = 50991
failures = 0


def check(ok, what, detail=""):
    global failures
    print(f"{'PASS' if ok else 'FAIL'}  {what}" + (f"  ({detail})" if detail else ""))
    if not ok:
        failures += 1


class Bench:
    def __init__(self, tx_delay_ms=300):
        env = dict(os.environ, TCI_BENCH_TX_DELAY_MS=str(tx_delay_ms))
        self.p = subprocess.Popen([os.path.join(HERE, "tci_bench"), str(PORT)], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        self.lines = []
        threading.Thread(target=self._read, daemon=True).start()
        for _ in range(50):
            if any("TCI on" in l for l in self.lines):
                return
            time.sleep(0.05)
        raise RuntimeError("bench didn't start: " + "".join(self.lines))

    def _read(self):
        for line in self.p.stdout:
            self.lines.append(line.rstrip())

    def mark(self):
        return len(self.lines)

    def saw(self, text, since, timeout=1.5):
        end = time.time() + timeout
        while time.time() < end:
            if any(text in l for l in self.lines[since:]):
                return True
            time.sleep(0.02)
        return False

    def find(self, prefix, since, timeout=2.5):
        end = time.time() + timeout
        while time.time() < end:
            for l in self.lines[since:]:
                if l.startswith(prefix):
                    return l
            time.sleep(0.05)
        return None

    def stop(self):
        self.p.terminate()
        self.p.wait()


class Client:
    def __init__(self):
        t0 = time.monotonic()
        self.ws = WebSocket("127.0.0.1", PORT)
        self.init = []
        while True:
            f = self.ws.recv(2.0)
            if f is None:
                break
            if f[0] == 1:
                self.init.append(f[1].decode())
                if self.init[-1] == "ready;":
                    break
        self.init_ms = int((time.monotonic() - t0) * 1000)
        self.text = []
        self.binary = []

    def send(self, s):
        self.ws.send(s)

    def pump(self, seconds, on_binary=None):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            f = self.ws.recv(max(0.0, end - time.monotonic()))
            if f is None:
                break
            if f[0] == 1:
                self.text.append(f[1].decode())
            elif f[0] == 2 and len(f[1]) >= 64:
                if on_binary:
                    on_binary(f[1], header(f[1]))
                else:
                    self.binary.append(f[1])

    def expect(self, msg, seconds=1.0):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if msg in self.text:
                self.text.remove(msg)
                return True
            self.pump(0.05)
        return False

    def drain(self):
        self.pump(0.2)
        self.text.clear()

    def close(self):
        self.ws.sock.close()


def strongest(iq, rate):
    n = 4096
    spec = fft(iq[-n:])
    k = max(range(n), key=lambda i: abs(spec[i]))
    return (k if k < n // 2 else k - n) * rate / n


def main():
    bench = Bench()
    try:
        run(bench)
    finally:
        bench.stop()
    bench = Bench(tx_delay_ms=2500)
    try:
        run_slow_switch(bench)
    finally:
        bench.stop()
    print(f"\n{'ALL PASSED' if not failures else f'{failures} FAILED'}")
    return failures


def run(bench):
    a = Client()
    problems = check_init(a.init, a.init_ms)
    check(not problems, f"init burst: {len(a.init)} messages, ready after {a.init_ms} ms", "; ".join(problems))
    check("device:zbitx;" in a.init and "protocol:ExpertSDR3,2.0;" in a.init, "identifies as ExpertSDR3 / zbitx")
    check("vfo:0,0,14074000;" in a.init and "modulation:0,usb;" in a.init, "burst carries the radio's state")
    b = Client()
    a.drain()
    b.drain()

    # --- control, echoed to everyone ---
    m = bench.mark()
    a.send("vfo:0,0,7074000;")
    check(a.expect("vfo:0,0,7074000;") and b.expect("vfo:0,0,7074000;") and bench.saw("radio: freq 7074000", m),
          "vfo set: radio tuned, echoed to both clients")
    m = bench.mark()
    b.send("VFO:0,0,7075000;")
    check(a.expect("vfo:0,0,7075000;") and bench.saw("radio: freq 7075000", m), "uppercase (Hamlib) command")
    a.drain(), b.drain()
    a.send("vfo:0,0;")
    ok_a = a.expect("vfo:0,0,7075000;")
    b.pump(0.3)
    check(ok_a and "vfo:0,0,7075000;" not in b.text, "a read is answered to the asker only")
    a.send("vfo:0,0,1000;")
    check(a.expect("vfo:0,0,7075000;"), "out-of-range vfo refused, echo keeps the frequency in force")
    m = bench.mark()
    a.send("modulation:0,digu;")
    check(a.expect("modulation:0,digu;") and b.expect("modulation:0,digu;") and bench.saw("radio: mode 5", m),
          "modulation digu -> DIGI")
    a.send("drive:0,25;")
    check(b.expect("drive:0,25;"), "drive")
    a.send("volume:-12;")
    check(a.expect("volume:-12;") and bench.saw("radio: volume 80", m), "volume -12 dB -> AUDIO 80, echoed exactly")
    a.send("mute:true;")
    check(a.expect("mute:true;") and a.expect("volume:-60;"), "mute sets AUDIO 0")
    a.send("mute:false;")
    check(a.expect("mute:false;") and a.expect("volume:-12;"), "unmute restores the volume")
    a.send("rit_offset:0,500;rit_enable:0,true;")
    check(b.expect("rit_offset:0,500;") and b.expect("rit_enable:0,true;"), "RIT offset and switch, two commands in one message")
    a.send("rit_enable:0,false;")
    check(a.expect("rit_enable:0,false;"), "RIT off")
    a.send("split_enable:true;")
    check(a.expect("split_enable:0,false;"), "split stays off (WSJT-X Improved's bare form)")
    a.send("cw_macros_speed:25;")
    check(b.expect("cw_macros_speed:25;") and b.expect("cw_keyer_speed:25;"), "CW speed")
    a.send("device;trx_count;")
    check(a.expect("device:zbitx;") and a.expect("trx_count:1;"), "identification re-read on request")
    a.send("rx_smeter:0,0;")
    check(a.expect("rx_smeter:0,0,-73;"), "S-meter read")

    # --- receive audio ---
    frames = []
    got = []

    def on_audio(data, h):
        if h["type"] == 1:
            frames.append(h)
            got.extend(samples(data, h)[::h["channels"]])

    a.drain()
    a.send("audio_start:0;")
    a.pump(2.0, on_audio)
    a.send("audio_stop:0;")
    if frames:
        h = frames[0]
        rate = len(got) / 2.0
        rms = math.sqrt(sum(v * v for v in got[len(got) // 4:]) / (len(got) * 3 // 4))
        hz = strongest([complex(v, 0) for v in got], 48000)
        check(h["rate"] == 48000 and h["format"] == 3 and h["channels"] == 2 and abs(rate - 48000) < 2500,
              "RX audio default: 48 kHz float32 stereo", f"{len(got)/2:.0f} samples/s, length {h['length']}")
        check(-20 < 20 * math.log10(rms) < -16 and abs(abs(hz) - 1000) < 30, "RX audio carries the 1 kHz tone at about -18 dBFS",
              f"{20*math.log10(rms):.1f} dBFS rms, {abs(hz):.0f} Hz")
    else:
        check(False, "RX audio frames arrived")

    frames.clear(), got.clear()
    a.send("audio_samplerate:12000;audio_stream_sample_type:int16;audio_stream_channels:1;")
    a.pump(0.3)
    a.send("audio_start:0;")
    a.pump(2.0, on_audio)
    a.send("audio_stop:0;")
    check(bool(frames) and frames[0]["rate"] == 12000 and frames[0]["format"] == 0 and frames[0]["channels"] == 1
          and abs(len(got) / 2.0 - 12000) < 1000, "RX audio 12 kHz int16 mono",
          f"{len(got)/2:.0f} samples/s" if frames else "none")

    # --- I/Q ---
    for rate in (48000, 96000):
        iq = []
        fr = []

        def on_iq(data, h):
            if h["type"] == 0:
                fr.append(h)
                s = samples(data, h)
                iq.extend(complex(s[k], s[k + 1]) for k in range(0, len(s) - 1, 2))

        a.drain()
        a.send(f"iq_samplerate:{rate};iq_start:0;")
        a.pump(1.5, on_iq)
        a.send("iq_stop:0;")
        if len(iq) >= 4096:
            hz = strongest(iq, rate)
            check(fr[0]["rate"] == rate and abs(len(iq) / 1.5 - rate) / rate < 0.1 and abs(hz - 1500) < 50,
                  f"I/Q at {rate}: signal at +1500 Hz (not mirrored)", f"{hz:+.0f} Hz, {len(iq)/1.5:.0f} samples/s")
        else:
            check(False, f"I/Q at {rate} arrived")

    # --- transmit with TCI audio ---
    a.drain(), b.drain()
    a.send("modulation:0,usb;")
    a.pump(0.2)
    m = bench.mark()
    phase = [0]
    answered = [0]

    def on_chrono(data, h):
        if h["type"] != 3:
            return
        n = h["length"]
        vals = []
        for _ in range(n // 2):
            v = 0.3 * math.sin(2 * math.pi * 700 * phase[0] / 48000)
            phase[0] += 1
            vals += [v, v]
        out = bytearray(data[:64])
        out[24:28] = struct.pack("<I", 2)
        a.ws.send(bytes(out) + struct.pack(f"<{n}f", *vals), opcode=2)
        answered[0] += 1

    a.send("trx:0,true,tci;")
    a.pump(0.6, on_chrono)
    check(bench.saw("radio: tx 1 (tci audio)", m) and b.expect("trx:0,true;"), "trx with tci source keys, echoed")
    check(bench.saw("radio: tx is now 1", m) and "trx:0,false;" not in b.text,
          "no trx:false flicker while the radio is still switching on")
    b.send("trx:0,false;")
    a.pump(0.3, on_chrono)
    check(not bench.saw("radio: tx 0", m, 0.2), "another client's trx is ignored while one holds TX")
    a.pump(2.2, on_chrono)
    line = bench.find("radio: tx audio rms", m)
    ok = False
    if line:
        rms = float(line.split("rms ")[1].split(",")[0])
        hz = float(line.split(", ")[1].split(" Hz")[0])
        ok = abs(rms - 0.3 / math.sqrt(2)) < 0.03 and abs(hz - 700) < 20
    check(ok, "TX audio reaches the mic input as the tone sent (0.3 peak, 700 Hz)", line or "no report")
    secs = 3.1  # from trx to here
    check(50 < answered[0] < secs * 48000 / 1024 + 15,
          "TX_CHRONO paced to real time (audio held, not discarded, while TX switches on)",
          f"{answered[0]} answered, ~{secs * 48000 / 1024:.0f} expected")
    a.send("trx:0,false;")
    a.pump(0.3, on_chrono)
    check(bench.saw("radio: tx 0", m), "trx false unkeys")

    # --- loopback PTT, refusal, disconnect while transmitting ---
    m = bench.mark()
    b.drain()
    b.send("trx:0,true;")
    check(b.expect("trx:0,true;") and bench.saw("radio: tx 1 (loopback)", m), "trx without source uses the loopback (as Hamlib PTT)")
    b.close()
    check(bench.saw("radio: tx 0", m, 2.0), "a client that disconnects while transmitting releases TX")
    a.drain()
    a.send("vfo:0,0,1000000;")
    a.pump(0.2)
    m = bench.mark()
    a.send("trx:0,true;")
    check(a.expect("trx:0,false;") and not bench.saw("radio: tx 1", m, 0.3), "TX refused by the radio is echoed as false")
    a.send("vfo:0,0,7030000;")
    a.pump(0.2)

    # --- CW text ---
    a.drain()
    m = bench.mark()
    a.send("cw_macros:0,CQ TEST;")
    check(not bench.saw("radio: cw queued", m, 0.3), "CW text ignored outside CW")
    a.send("modulation:0,cw;")
    a.pump(0.2)
    a.send("cw_macros:0,cq_test de n0call;")
    check(bench.saw('radio: cw queued "CQ TEST DE N0CALL"', m), "cw_macros queued, uppercase, '_' as space")
    check(a.expect("cw_macros_empty;", 3.0), "cw_macros_empty when it has been sent")
    a.send("cw_msg:0,TU,K1ABC,5NN;")
    check(bench.saw('radio: cw queued "TU K1ABC 5NN"', m), "cw_msg joins its parts")
    a.send("cw_macros_stop;")
    check(bench.saw("radio: cw stop", m) and a.expect("cw_macros_stop;"), "cw_macros_stop")
    a.close()


def run_slow_switch(bench):
    """The radio takes 2.5 s to follow a TX request (a busy UI thread)."""
    print("-- radio slow to switch (2.5 s):")
    a = Client()
    b = Client()
    a.drain(), b.drain()
    m = bench.mark()
    a.send("trx:0,true;")
    a.pump(2.0)
    check("trx:0,true;" in a.text and "trx:0,false;" not in a.text and "trx:0,false;" not in b.text,
          "keyed: echoed true at once, no false while the radio catches up")
    a.send("trx:0,false;")
    check(a.expect("trx:0,false;") and bench.saw("radio: tx 0", m), "the owner can unkey before the radio has even keyed")
    check(bench.saw("radio: tx is now 0", m, 6.0), "the radio ends up unkeyed")
    m = bench.mark()
    a.drain()
    a.send("trx:0,true;")
    a.pump(1.0)
    a.close()
    check(bench.saw("radio: tx 0", m, 2.0) and bench.saw("radio: tx is now 0", m, 8.0),
          "a client that vanishes mid-switch still leaves the radio unkeyed")
    b.close()


if __name__ == "__main__":
    sys.exit(main())
