# TCI server

zBitx runs a TCI server (Expert Electronics' Transceiver Control
Interface, v2.0) on TCP port 50001. A TCI connection carries the radio's
control, its receive and transmit audio, its I/Q and CW text over a single
network link, so apps such as JTDX, WSJT-X Improved, MSHV, loggers and
sdrOxide need no audio loopback and no separate CAT connection. Up to 4
clients can connect at once, and every change is sent to all of them,
including changes made from the front panel, the web UI or Hamlib.

The protocol, stream and WebSocket code comes from maxibitx
(https://github.com/MikeJohnshoy/maxibitx).

## What it does

| TCI | zBitx |
|---|---|
| `vfo` / `dds` / `if` | `vfo` is the dial frequency (FREQ), 0.5 to 32 MHz. `vfo = dds + if`: an SDR client such as SDR oxide holds the I/Q center (`dds`) and moves its VFO with `if`. zBitx tunes to the VFO, where it demodulates and transmits, and shifts TCI's I/Q so it stays centered on `dds`. A dial change from the panel, web or Hamlib resets `if` to 0 |
| `modulation` | usb, lsb, cw, cwr, am, digu (= DIGI). FT8 mode reports as digu |
| `trx` | PTT, held until `trx:0,false`. `trx:0,true,tci` sends the client's own audio, in every mode including CW/CWR, where that audio is the client's keyed sidetone (SDR oxide's keyboard CW). Otherwise it sends the loopback audio, like Hamlib PTT; in CW it keys zbitx's keyer and holds it |
| `drive` | DRIVE, 0 to 100 |
| `volume` / `mute` | AUDIO: -60..0 dB maps to 0..100, and mute sets 0 |
| `rit_enable` / `rit_offset` | RIT and RIT_DELTA |
| `cw_macros_speed` / `cw_keyer_speed` | WPM |
| `cw_macros`, `cw_msg`, `cw_macros_stop` | text for zbitx's keyer, in CW or CWR only. `cw_macros_empty` is sent once the keyer has finished sending it, including the last character |
| `rx_smeter`, `rx_sensors_enable` | the S-meter (uncalibrated: S9 = -73 dBm) |
| `audio_start` | receive audio at 48, 24, 12 or 8 kHz, as int16/24/32 or float32, in mono or stereo |
| `iq_start` | I/Q from the same front end as HPSDR, at 48 kHz (the default) or 96 kHz, not mirrored |

`split`, `xit` and `tune` always report off, and so do the DSP switches.
zBitx has one VFO as far as TCI is concerned.

## Setting up an app

- **JTDX / WSJT-X Improved:** set Radio to *TCI Client RX1*, with server
  `<zbitx-ip>:50001`. Set PTT method to *CAT*. Set both the input and output
  audio devices to *TCI audio*.
- **SDR oxide and other panadapters:** point them at `<zbitx-ip>:50001`. They
  ask for I/Q themselves. SDR oxide sends keyboard CW as its own keyed
  sidetone over TCI audio. zBitx transmits that audio in CW mode, mapped so
  the carrier lands on the dial when SDR oxide's CW pitch matches zBitx's
  PITCH. Set both to the same value.
- **Hamlib's TCI backend:** don't use it. As of Hamlib 4.6.2, and the
  development branch on 2026-10-11, the backend is disabled and unfinished:
  it sends flrig-style commands that TCI doesn't have. Use Hamlib's rigctld
  on port 4532 instead.

## Settings (data/hw_settings.ini, any line)

| Key | Default | Meaning |
|---|---|---|
| `tci_port` | 50001 | TCP port. 0 turns the server off |
| `tci_bind` | (all) | the IPv4 address to listen on, such as 127.0.0.1 to allow apps on the Pi only |

Set `ZBITX_TCI_TRACE=1` in the environment to print every TCI message on
the console.

## How it fits into zbitx

- `tci.c` handles the protocol: the startup burst, the commands, and echoing
  each change to every client. The service thread runs every 5 ms. Every
  50 ms it checks the radio's state and sends anything that changed.
- `tci_radio.c` is the only file that touches zbitx. It reads the fields
  directly. It hands every change to the UI thread, through a queue that
  `ui_tick()` drains, because the UI thread owns the fields, the text sender
  and TX switching. For settings, the TCI thread waits up to 250 ms for the
  UI thread to apply the change, so the echo carries the value actually in
  force.
- In CW, `tx_on()` normally hands TX to the keyer's runtime, which drops it
  after the CW delay. A client sending its own audio is keyed instead with
  `tx_on_audio()` (sbitx_main.c), the normal T/R path: TX holds until the
  client unkeys, and `tx_process()` sends the client's audio at the keyer's
  key-down level, not the keyer's output. A client keying CW without audio
  holds the keyer's runtime up with `cw_runtime_request_tx()` every 5 ms for
  as long as it holds `trx`. Its `trx:0,false` doesn't call `tx_off()`,
  which would abort text mid-character: the keyer finishes and drops TX
  after its hang time.
- If the radio drops TX under a client (panel, timeout, another control),
  clients are sent `trx:0,false`. The client's TX stream keeps being paced
  with `TX_CHRONO` until the client unkeys, because clients such as SDR
  oxide stall without them. Its audio is consumed and discarded, and never
  reaches the mic input of a transmission that someone else keyed.
- TX switching is never waited on. `tx_on()` saves settings and waits for the
  relays to settle, and the UI thread can be busy for seconds (FFTW planning
  after a mode change). TCI therefore echoes `trx:0,true` straight away and
  keeps the TX state it reports steady until the radio catches up.
  - A client holds TX until it unkeys or disconnects. Either way zbitx is
    unkeyed, even if the radio hasn't keyed yet.
  - TX keyed by something else (mic PTT, Hamlib, the panel) is never unkeyed
    from TCI.
- `tci_audio.c` runs on the audio thread:
  - **Receive audio** is the speaker audio after AGC. AUDIO only sets the
    codec's headphone level, so it doesn't change what TCI gets. The audio
    is decimated to 48 kHz and scaled so the AGC's peaks sit about 15 dB
    below full scale.
  - **Transmit audio** from the client is upsampled to 96 kHz and written
    over the mic input before `tx_process()` reads it, at the same
    full-scale level as the loopback. Audio that arrives before zbitx has
    switched to TX waits in the queue, for up to 750 ms, so the start of a
    transmission isn't cut off. After that the audio is consumed in real
    time so the client stays paced.
- `tci_stream.c` and `tci_ws.c` are maxibitx's, unchanged except for the I/Q
  sign and the audio scale.

## Testing without a radio

    make test-tci          # 54 scripted checks against a stub radio (tests/tci_test.py),
                           # including SDR oxide's keyboard-CW sequence
    make tci-bench && ./tests/tci_bench    # a stub server, for trying a real app
    python3 tests/tci_client.py --host <zbitx-ip>              # the startup burst, checked
    python3 tests/tci_client.py --host <zbitx-ip> --audio 3 --iq 3
    python3 tests/tci_client.py --host <zbitx-ip> -c "vfo:0,0,7074000;"
    python3 tests/tci_client.py --host <zbitx-ip> --tx-tone 1000   # TRANSMITS: use a dummy load

The bench's stub radio switches TX 300 ms after each request, as zBitx can.
The test also runs it with a 2.5 s delay.
