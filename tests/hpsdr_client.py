#!/usr/bin/env python3
"""Minimal openHPSDR Protocol 1 client for testing the zBitx's HPSDR server.

Works against the off-radio bench (tests/hpsdr_bench.c) or a real zBitx:

  python3 tests/hpsdr_client.py                       # discover by broadcast
  python3 tests/hpsdr_client.py --host 192.168.1.50 --freq 7074000 --seconds 5
  python3 tests/hpsdr_client.py --host 127.0.0.1 --rate 48000 --nrx 2
  python3 tests/hpsdr_client.py --host zbitx.local --wav iq.wav   # save I/Q

Reports discovery fields, packet rate, the effective sample rate, sequence
gaps, and (with numpy) the strongest signal in the I/Q and which side of
center it is on. --wav writes the I/Q as a stereo WAV (L=I, R=Q) that SDR
apps and Audacity can open. Exit status is non-zero if streaming failed or
a --expect-tone check fails.
"""
import argparse
import socket
import struct
import sys
import time
import wave

RATES = {48000: 0, 96000: 1, 192000: 2, 384000: 3}


def discover(host, timeout=1.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(timeout)
    pkt = bytes([0xEF, 0xFE, 0x02]) + bytes(60)
    s.sendto(pkt, (host or "255.255.255.255", 1024))
    found = []
    t_end = time.time() + timeout
    while time.time() < t_end:
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            break
        if len(data) >= 11 and data[0] == 0xEF and data[1] == 0xFE and data[2] in (2, 3):
            found.append((addr[0], data))
            if host:
                break
    s.close()
    return found


def ep2_packet(seq, frames):
    out = struct.pack(">BBBBI", 0xEF, 0xFE, 0x01, 0x02, seq)
    for c0, c in frames:
        fr = bytes([0x7F, 0x7F, 0x7F, c0]) + bytes(c) + bytes(504)
        out += fr
    return out


def cc(addr, mox, c1to4):
    return ((addr << 1) | (1 if mox else 0), c1to4)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", help="radio IP (default: broadcast discovery)")
    ap.add_argument("--rate", type=int, default=96000, choices=sorted(RATES))
    ap.add_argument("--nrx", type=int, default=1, choices=range(1, 9))
    ap.add_argument("--freq", type=int, default=0, help="RX1 frequency to send, Hz")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--mox", type=float, default=0.0, help="hold MOX for this many seconds mid-run")
    ap.add_argument("--no-stop", action="store_true", help="go silent instead of sending STOP")
    ap.add_argument("--expect-tone", type=float, help="fail unless the strongest signal is within 100 Hz of this offset")
    ap.add_argument("--wav", help="save received I/Q (receiver 1) as a stereo WAV")
    a = ap.parse_args()

    found = discover(a.host)
    if not found:
        print("no HPSDR radio answered discovery")
        return 2
    ip, d = found[0]
    status = {2: "idle", 3: "busy (streaming to another app)"}.get(d[2], hex(d[2]))
    print(f"discovered {ip}: status {status}, MAC {':'.join('%02X' % b for b in d[3:9])}, "
          f"firmware 0x{d[9]:02X}, board id {d[10]}")

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    s.settimeout(0.5)
    dest = (ip, 1024)
    s.sendto(bytes([0xEF, 0xFE, 0x04, 0x01]) + bytes(60), dest)

    c1 = RATES[a.rate]
    c4 = (a.nrx - 1) << 3
    seq = 0
    pkts, gaps, last_seq, ptt_seen = 0, 0, None, 0
    spf = 504 // (6 * a.nrx + 2)
    iq = []
    t0 = time.time()
    t_first = None
    next_ep2 = t0
    mox_start = t0 + (a.seconds - a.mox) / 2 if a.mox else None
    while time.time() - t0 < a.seconds:
        now = time.time()
        mox = bool(mox_start and mox_start <= now < mox_start + a.mox)
        if now >= next_ep2:
            # Rotate C&C addresses 0 (config + MOX), 1 (TX freq), 2 (RX1 freq).
            frames = [cc(0, mox, [c1, 0, 0, c4])]
            frames.append(cc(2, mox, struct.pack(">I", a.freq)) if a.freq and seq % 2 == 0
                          else cc(1, mox, struct.pack(">I", a.freq)))
            s.sendto(ep2_packet(seq, frames), dest)
            seq += 1
            next_ep2 = now + 0.01
        try:
            data, _ = s.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) != 1032 or data[:4] != b"\xEF\xFE\x01\x06":
            continue
        if t_first is None:
            t_first = time.time()
        pkts += 1
        sq = struct.unpack(">I", data[4:8])[0]
        if last_seq is not None and sq != last_seq + 1:
            gaps += 1
        last_seq = sq
        for f in range(2):
            fp = data[8 + 512 * f:]
            if fp[0:3] != b"\x7F\x7F\x7F":
                gaps += 1
                continue
            ptt_seen |= fp[3] & 1
            p = 8
            for _ in range(spf):
                i = int.from_bytes(fp[p:p + 3], "big", signed=True)
                q = int.from_bytes(fp[p + 3:p + 6], "big", signed=True)
                iq.append((i, q))
                p += 6 * a.nrx + 2

    if a.no_stop:
        print("going silent without STOP (server should time out in ~3 s)")
    else:
        s.sendto(bytes([0xEF, 0xFE, 0x04, 0x00]) + bytes(60), dest)

    if not pkts:
        print("FAIL: no EP6 packets received")
        return 1
    dur = time.time() - t_first
    print(f"received {pkts} packets in {dur:.2f} s = {pkts / dur:.0f}/s, "
          f"{len(iq) / dur:.0f} samples/s (asked {a.rate}), sequence gaps {gaps}, "
          f"PTT bit seen {ptt_seen}")

    rc = 0
    rate_ok = abs(len(iq) / dur - min(a.rate, 96000)) / min(a.rate, 96000) < 0.05
    if not rate_ok:
        print("FAIL: sample rate off by more than 5%")
        rc = 1

    if a.wav:
        with wave.open(a.wav, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(3)
            w.setframerate(min(a.rate, 96000))
            w.writeframes(b"".join(struct.pack("<i", i)[:3] + struct.pack("<i", q)[:3] for i, q in iq))
        print(f"wrote {len(iq)} I/Q samples to {a.wav}")

    try:
        import numpy as np
    except ImportError:
        print("(install numpy for the spectrum check)")
        return rc
    x = np.array(iq[len(iq) // 4:], dtype=float)
    z = (x[:, 0] + 1j * x[:, 1]) / 8388607.0
    n = 1 << int(np.log2(len(z)))
    z = z[-n:] * np.hanning(n)
    spec = np.fft.fftshift(np.abs(np.fft.fft(z)) / (n / 2))
    freqs = np.fft.fftshift(np.fft.fftfreq(n, 1 / min(a.rate, 96000)))
    db = 20 * np.log10(spec + 1e-12)
    rate = min(a.rate, 96000)
    # Only the crystal filter's passband carries signal; beyond ~+-15 kHz is
    # the I/Q lowpass's stopband, so the floor and peaks are measured inside.
    inband = np.abs(freqs) <= min(12000, rate * 0.25)
    fi, di = freqs[inband], db[inband]
    k = int(np.argmax(di))
    floor_bin = float(np.median(di))
    bin_hz = rate / n
    # Noise density from the median in-band bin. With the /(n/2) amplitude
    # scaling a full-scale tone reads 0 dBFS; a Hann window's noise bandwidth
    # is 1.5 bins, and the median of noise-only bins sits ~1.6 dB below the mean.
    density = floor_bin + 1.6 - 10 * np.log10(1.5 * bin_hz)
    quant = 20 * np.log10(1 / 8388607) + 10 * np.log10(2 / 12) - 10 * np.log10(rate)  # 24-bit I+Q, dBFS/Hz
    mirror = di[np.argmin(np.abs(fi + fi[k]))]
    print(f"strongest in-band signal {fi[k]:+.0f} Hz from center at {di[k]:.1f} dBFS; "
          f"its mirror {mirror:.1f} dBFS")
    # strongest few distinct peaks, for a quick look at the band
    order = np.argsort(di)[::-1]
    peaks = []
    for j in order:
        if all(abs(fi[j] - p) > 150 for p in peaks):
            peaks.append(fi[j])
        if len(peaks) == 6:
            break
    print("top peaks (Hz from center):", ", ".join(f"{p:+.0f}" for p in sorted(peaks)))
    print(f"in-band noise floor {floor_bin:.1f} dBFS/bin ({bin_hz:.2f} Hz bins), "
          f"about {density:.0f} dBFS/Hz; 24-bit quantization is {quant:.0f} dBFS/Hz, "
          f"so the radio's noise sits {density - quant:.0f} dB above it")
    if a.expect_tone is not None:
        if abs(fi[k] - a.expect_tone) > 100:
            print(f"FAIL: expected strongest signal at {a.expect_tone:+.0f} Hz")
            rc = 1
        else:
            print("tone check PASS")
    return rc


if __name__ == "__main__":
    sys.exit(main())
